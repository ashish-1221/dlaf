#include <boost/function_output_iterator.hpp>
#include <boost/geometry/geometry.hpp>
#include <boost/thread.hpp>
#include <boost/thread/barrier.hpp>
#include <boost/thread/mutex.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <random>
#include <vector>

// number of dimensions (must be 2 or 3)
const int D = 3;

// number of worker threads
const int NumThreads = 16;
const int BatchSize = 128;

// default parameters (documented below)
const double DefaultParticleSpacing = 1;
const double DefaultAttractionDistance = 3;
const double DefaultMinMoveDistance = 1;
const double DefaultStickiness = 1;

// boost is used for its spatial index
using BoostPoint = boost::geometry::model::point<
    double, D, boost::geometry::cs::cartesian>;

using IndexValue = std::pair<BoostPoint, int>;

using Index = boost::geometry::index::rtree<
    IndexValue, boost::geometry::index::rstar<64>>;

typedef struct {
    uint32_t ParentID;
    float X;
    float Y;
    float Z;
} Record;

// Vector represents a point or a vector
class Vector {
public:
    Vector() :
        m_X(0), m_Y(0), m_Z(0) {}

    Vector(double x, double y) :
        m_X(x), m_Y(y), m_Z(0) {}

    Vector(double x, double y, double z) :
        m_X(x), m_Y(y), m_Z(z) {}

    Vector(BoostPoint p) :
        m_X(p.get<0>()), m_Y(p.get<1>()), m_Z(p.get<2>()) {}

    double X() const {
        return m_X;
    }

    double Y() const {
        return m_Y;
    }

    double Z() const {
        return m_Z;
    }

    BoostPoint ToBoost() const {
        return BoostPoint(m_X, m_Y, m_Z);
    }

    double Length() const {
        return std::sqrt(m_X * m_X + m_Y * m_Y + m_Z * m_Z);
    }

    double LengthSquared() const {
        return m_X * m_X + m_Y * m_Y + m_Z * m_Z;
    }

    double Distance(const Vector &v) const {
        const double dx = m_X - v.m_X;
        const double dy = m_Y - v.m_Y;
        const double dz = m_Z - v.m_Z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    double DistanceSquared(const Vector &v) const {
        const double dx = m_X - v.m_X;
        const double dy = m_Y - v.m_Y;
        const double dz = m_Z - v.m_Z;
        return dx * dx + dy * dy + dz * dz;
    }

    Vector Normalized() const {
        const double m = 1 / Length();
        return Vector(m_X * m, m_Y * m, m_Z * m);
    }

    Vector operator+(const Vector &v) const {
        return Vector(m_X + v.m_X, m_Y + v.m_Y, m_Z + v.m_Z);
    }

    Vector operator-(const Vector &v) const {
        return Vector(m_X - v.m_X, m_Y - v.m_Y, m_Z - v.m_Z);
    }

    Vector operator*(const double a) const {
        return Vector(m_X * a, m_Y * a, m_Z * a);
    }

    Vector &operator+=(const Vector &v) {
        m_X += v.m_X; m_Y += v.m_Y; m_Z += v.m_Z;
        return *this;
    }

private:
    double m_X;
    double m_Y;
    double m_Z;
};

// Lerp linearly interpolates from a to b by distance.
Vector Lerp(const Vector &a, const Vector &b, const double d) {
    return a + (b - a).Normalized() * d;
}

// Random returns a uniformly distributed random number between lo and hi
double Random(const double lo = 0, const double hi = 1) {
    static thread_local std::mt19937 gen(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(gen);
}

// RandomInUnitSphere returns a random, uniformly distributed point inside the
// unit sphere (radius = 1)
Vector RandomInUnitSphere() {
    while (true) {
        const Vector p = Vector(
            Random(-1, 1),
            Random(-1, 1),
            D == 2 ? 0 : Random(-1, 1));
        if (p.LengthSquared() < 1) {
            return p;
        }
    }
}

// Model holds all of the particles and defines their behavior.
class Model {
public:
    Model() :
        m_ParticleSpacing(DefaultParticleSpacing),
        m_AttractionDistance(DefaultAttractionDistance),
        m_MinMoveDistance(DefaultMinMoveDistance),
        m_Stickiness(DefaultStickiness),
        m_BoundingRadius(0) {}

    void SetParticleSpacing(const double a) {
        m_ParticleSpacing = a;
    }

    void SetAttractionDistance(const double a) {
        m_AttractionDistance = a;
    }

    void SetMinMoveDistance(const double a) {
        m_MinMoveDistance = a;
    }

    void SetStickiness(const double a) {
        m_Stickiness = a;
    }

    // Add adds a new particle with the specified parent particle
    void Add(const Vector &p, const int parent = -1) {
        const int id = m_Index.size();
        m_Index.insert(std::make_pair(p.ToBoost(), id));
        m_BoundingRadius = std::max(
            m_BoundingRadius, p.Length() + m_AttractionDistance);
        Record record = {
            static_cast<uint32_t>(parent),
            static_cast<float>(p.X()),
            static_cast<float>(p.Y()),
            static_cast<float>(p.Z()),
        };
        fwrite(&record, sizeof(Record), 1, stdout);
    }

    // Nearest returns the particle nearest the specified point
    IndexValue Nearest(const Vector &point) const {
        IndexValue result;
        m_Index.query(
            boost::geometry::index::nearest(point.ToBoost(), 1),
            boost::make_function_output_iterator([&result](const auto &value) {
                result = value;
            }));
        return result;
    }

    // RandomStartingPosition returns a random point to start a new particle
    Vector RandomStartingPosition() const {
        const double d = m_BoundingRadius;
        return RandomInUnitSphere().Normalized() * d;
    }

    // ShouldReset returns true if the particle has gone too far away and
    // should be reset to a new random starting position
    bool ShouldReset(const Vector &p) const {
        return p.Length() > m_BoundingRadius * 2;
    }

    // ShouldJoin returns true if the point should attach to the specified
    // parent particle. This is only called when the point is already within
    // the required attraction distance.
    bool ShouldJoin(const Vector &p, const IndexValue &parent) {
        return Random() <= m_Stickiness;
    }

    // PlaceParticle computes the final placement of the particle.
    Vector PlaceParticle(const Vector &p, const Vector &parent) const {
        return Lerp(parent, p, m_ParticleSpacing);
    }

    // MotionVector returns a vector specifying the direction that the
    // particle should move for one iteration. The distance that it will move
    // is determined by the algorithm.
    Vector MotionVector(const Vector &p) const {
        return RandomInUnitSphere();
    }

    // Walk diffuses one new particle
    std::pair<Vector, int> Walk() {
        // compute particle starting location
        Vector p = RandomStartingPosition();

        // do the random walk
        while (true) {
            // get distance to nearest other particle
            const IndexValue parent = Nearest(p);
            const Vector parentPoint(parent.first);
            const double d = p.Distance(parentPoint);

            // check if close enough to join
            if (d < m_AttractionDistance) {
                if (!ShouldJoin(p, parent)) {
                    // push particle away a bit
                    p = Lerp(parentPoint, p,
                        m_AttractionDistance + m_MinMoveDistance);
                    continue;
                }

                // adjust particle position in relation to its parent
                p = PlaceParticle(p, parentPoint);

                // return the new particle position and its parent
                return std::make_pair(p, parent.second);
            }

            // move randomly
            const double m = std::max(
                m_MinMoveDistance, d - m_AttractionDistance);
            p += MotionVector(p).Normalized() * m;

            // check if particle is too far away, reset if so
            if (ShouldReset(p)) {
                p = RandomStartingPosition();
            }
        }
    }

    void RunForever(const int numThreads, const int batchSize) {
        boost::barrier barrier1(numThreads + 1);
        boost::barrier barrier2(numThreads + 1);
        boost::mutex mutex;

        std::vector<std::pair<Vector, int>> items;
        const double threshold = std::pow(m_AttractionDistance * 5, 2);

        const auto worker = [&]() {
            while (1) {
                barrier1.wait();
                bool done = false;
                while (!done) {
                    const auto item = Walk();
                    mutex.lock();
                    items.push_back(item);
                    done = items.size() >= batchSize;
                    mutex.unlock();
                }
                barrier2.wait();
            }
        };

        std::vector<boost::thread> threads;
        for (int i = 0; i < numThreads; i++) {
            threads.emplace_back(worker);
        }

        while (1) {
            barrier1.wait();
            barrier2.wait();
            for (int i = 0; i < items.size(); i++) {
                bool ok = true;
                for (int j = 0; j < i; j++) {
                    const double d2 = items[i].first.DistanceSquared(items[j].first);
                    if (d2 < threshold) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    Add(items[i].first, items[i].second);
                }
            }
            items.clear();
        }
    }

private:
    // m_ParticleSpacing defines the distance between particles that are
    // joined together
    double m_ParticleSpacing;

    // m_AttractionDistance defines how close together particles must be in
    // order to join together
    double m_AttractionDistance;

    // m_MinMoveDistance defines the minimum distance that a particle will move
    // during its random walk
    double m_MinMoveDistance;

    // m_Stickiness defines the probability that a particle will allow another
    // particle to join to it.
    double m_Stickiness;

    // m_BoundingRadius defines the radius of the bounding sphere that bounds
    // all of the particles
    double m_BoundingRadius;

    // m_Index is the spatial index used to accelerate nearest neighbor queries
    Index m_Index;
};

int main() {
    // create the model
    Model model;

    // add seed point(s)
    int count = 0;
    while (1) {
        Record r;
        const int n = fread(&r, sizeof(Record), 1, stdin);
        if (n == 0) {
            break;
        }
        model.Add(Vector(r.X, r.Y, r.Z), r.ParentID);
        count++;
    }

    if (count == 0) {
        model.Add(Vector());
    }

    model.RunForever(NumThreads, BatchSize);

    return 0;
}
