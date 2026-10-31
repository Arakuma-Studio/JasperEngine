// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "PolygonDecomposition2D.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

using namespace jasperengine;

namespace {

    struct Point {
        double x;
        double y;
    };

    // Twice the signed area of (a, b, c): > 0 when c is left of a->b.
    inline double orient(const Point& a, const Point& b, const Point& c) {
        return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    }

    inline double distance2(const Point& a, const Point& b) {
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        return dx * dx + dy * dy;
    }

    inline bool samePoint(const Point& a, const Point& b) {
        return a.x == b.x && a.y == b.y;
    }

    // Twice the signed area of the polygon, relative to its first point.
    double area2(const std::vector<Point>& p, const std::vector<int>& ring) {
        double sum = 0.0;
        for (size_t i = 1; i + 1 < ring.size(); i++) {
            sum += orient(p[ring[0]], p[ring[i]], p[ring[i + 1]]);
        }
        return sum;
    }

    // Drops points closer than the weld distance to the previous kept one (exact duplicates
    // always), then the closing duplicates.
    void weldRing(const std::vector<Point>& p, std::vector<int>& ring, double weld2, std::vector<int>& scratch) {
        auto tooClose = [&](int a, int b) {
            const double d2 = distance2(p[a], p[b]);
            return d2 == 0.0 || d2 < weld2;
        };
        scratch.clear();
        for (int index : ring) {
            if (scratch.empty() || !tooClose(scratch.back(), index)) {
                scratch.push_back(index);
            }
        }
        while (scratch.size() > 1 && tooClose(scratch.back(), scratch.front())) {
            scratch.pop_back();
        }
        ring.swap(scratch);
    }

    // Removes points where the outline goes straight on (or folds back on itself): a point goes
    // when it, and every point removed next to it, is closer than `tolerance` (a distance, like
    // b2ComputeHull's linear slop) to the line between the kept points around them. Local and
    // absolute, so a long outline keeps its small detail (hills on a 3 km terrain, comb teeth).
    void removeCollinear(const std::vector<Point>& p, std::vector<int>& ring, double tolerance, std::vector<int>& scratch) {
        const size_t n = ring.size();
        if (n < 3) {
            return;
        }
        // start at the lowest point (then leftmost): a hull vertex, kept
        size_t first = 0;
        for (size_t i = 1; i < n; i++) {
            const Point& a = p[ring[i]];
            const Point& b = p[ring[first]];
            if (a.y < b.y || (a.y == b.y && a.x < b.x)) {
                first = i;
            }
        }
        auto at = [&](size_t position) {
            return ring[(first + position) % n];
        };
        // the points between positions a and c (exclusive) all lie within tolerance of a-c
        auto straight = [&](size_t a, size_t c) {
            const Point& pa = p[at(a)];
            const Point& pc = p[at(c)];
            const double length = std::sqrt(distance2(pa, pc));
            for (size_t b = a + 1; b < c; b++) {
                if (std::fabs(orient(pa, p[at(b)], pc)) > tolerance * length) {
                    return false;
                }
            }
            return true;
        };
        // positions kept, the closing copy of the first one at n
        std::vector<size_t> kept;
        kept.reserve(n + 1);
        kept.push_back(0);
        for (size_t position = 1; position <= n; position++) {
            kept.push_back(position);
            while (kept.size() >= 3 && straight(kept[kept.size() - 3], kept.back())) {
                kept.erase(kept.end() - 2);
            }
        }
        kept.pop_back();
        scratch.clear();
        for (size_t position : kept) {
            scratch.push_back(at(position));
        }
        ring.swap(scratch);
    }

    // Closed segments a-b and c-d cross or touch.
    bool segmentsTouch(const Point& a, const Point& b, const Point& c, const Point& d) {
        const double d1 = orient(c, d, a);
        const double d2 = orient(c, d, b);
        const double d3 = orient(a, b, c);
        const double d4 = orient(a, b, d);
        if (((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
            ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0))) {
            return true;
        }
        // the caller checked that the bounding boxes overlap
        auto onSegment = [](const Point& s, const Point& e, const Point& q) {
            return q.x >= std::min(s.x, e.x) && q.x <= std::max(s.x, e.x) &&
                   q.y >= std::min(s.y, e.y) && q.y <= std::max(s.y, e.y);
        };
        return (d1 == 0.0 && onSegment(c, d, a)) || (d2 == 0.0 && onSegment(c, d, b)) ||
               (d3 == 0.0 && onSegment(a, b, c)) || (d4 == 0.0 && onSegment(a, b, d));
    }

    // No two non-adjacent edges cross or touch.
    bool isSimple(const std::vector<Point>& q) {
        const size_t n = q.size();
        for (size_t i = 0; i < n; i++) {
            const Point& a = q[i];
            const Point& b = q[(i + 1) % n];
            const double minX = std::min(a.x, b.x);
            const double maxX = std::max(a.x, b.x);
            const double minY = std::min(a.y, b.y);
            const double maxY = std::max(a.y, b.y);
            for (size_t j = i + 2; j < n; j++) {
                if (i == 0 && j == n - 1) {
                    continue;
                }
                const Point& c = q[j];
                const Point& d = q[(j + 1) % n];
                if (std::max(c.x, d.x) < minX || std::min(c.x, d.x) > maxX ||
                    std::max(c.y, d.y) < minY || std::min(c.y, d.y) > maxY) {
                    continue;
                }
                if (segmentsTouch(a, b, c, d)) {
                    return false;
                }
            }
        }
        return true;
    }

    // Andrew's monotone chain, counter-clockwise from the lowest x (then lowest y), collinear
    // points left out. Returns indices into q.
    std::vector<int> convexHull(const std::vector<Point>& q) {
        std::vector<int> order(q.size());
        for (size_t i = 0; i < q.size(); i++) {
            order[i] = (int)i;
        }
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            if (q[a].x != q[b].x) return q[a].x < q[b].x;
            if (q[a].y != q[b].y) return q[a].y < q[b].y;
            return a < b;
        });
        std::vector<int> hull(2 * q.size() + 1);
        size_t k = 0;
        for (int index : order) {
            while (k >= 2 && orient(q[hull[k - 2]], q[hull[k - 1]], q[index]) <= 0.0) {
                k--;
            }
            hull[k++] = index;
        }
        const size_t lower = k + 1;
        for (size_t i = order.size(); i-- > 1;) {
            const int index = order[i - 1];
            while (k >= lower && orient(q[hull[k - 2]], q[hull[k - 1]], q[index]) <= 0.0) {
                k--;
            }
            hull[k++] = index;
        }
        hull.resize(k > 0 ? k - 1 : 0);
        return hull;
    }

    struct Triangle {
        int a;
        int b;
        int c;
    };

    struct Diagonal {
        int a;
        int c;
        int first;     // the ear that created it (holds the edge c->a)
        int second;    // the later triangle on the other side (holds a->c), -1 if none
    };

    class EarClipper {
    public:
        EarClipper(const std::vector<Point>& points): p(points) {}

        // Triangulates the simple counter-clockwise polygon p. Returns the triangles that could
        // not be built (degenerate leftovers).
        int run() {
            const int n = (int)p.size();
            prev.resize(n);
            next.resize(n);
            edgeDiagonal.assign(n, -1);
            reflexSlot.assign(n, -1);
            reflex.clear();
            reflex.reserve(n);
            triangles.clear();
            triangles.reserve(n);
            diagonals.clear();
            diagonals.reserve(n);
            for (int i = 0; i < n; i++) {
                prev[i] = (i + n - 1) % n;
                next[i] = (i + 1) % n;
            }
            for (int i = 0; i < n; i++) {
                updateReflex(i);
            }

            int dropped = 0;
            int remaining = n;
            int current = 0;
            int misses = 0;
            while (remaining > 3) {
                const int a = prev[current];
                const int c = next[current];
                const double turn = orient(p[a], p[current], p[c]);
                if (turn == 0.0 &&
                    (p[current].x - p[a].x) * (p[c].x - p[current].x) + (p[current].y - p[a].y) * (p[c].y - p[current].y) > 0.0) {
                    // straight on: the vertex goes without a triangle (its edges' diagonals keep
                    // a single side)
                    unlink(current);
                    edgeDiagonal[a] = -1;
                    remaining--;
                    updateReflex(a);
                    updateReflex(c);
                    current = c;
                    misses = 0;
                    continue;
                }
                // a whole turn without an ear (rounding): take the next convex vertex anyway
                if (turn > 0.0 && (misses >= remaining || isEar(a, current, c))) {
                    clip(a, current, c);
                    remaining--;
                    current = c;
                    misses = 0;
                    continue;
                }
                current = c;
                misses++;
                if (misses >= 2 * remaining) {
                    // nothing convex is left: the rest has no area
                    return dropped + 1;
                }
            }
            const int a = prev[current];
            const int c = next[current];
            if (orient(p[a], p[current], p[c]) > 0.0) {
                const int t = (int)triangles.size();
                triangles.push_back({a, current, c});
                attach(a, t);
                attach(current, t);
                attach(c, t);
            } else {
                dropped++;
            }
            return dropped;
        }

        std::vector<Triangle> triangles;
        std::vector<Diagonal> diagonals;

    private:
        const std::vector<Point>& p;
        std::vector<int> prev;
        std::vector<int> next;
        std::vector<int> edgeDiagonal;   // the diagonal on the edge v -> next[v], -1 if none
        std::vector<int> reflex;         // vertices that are not strictly convex
        std::vector<int> reflexSlot;     // index of the vertex in reflex, -1 if not there

        void updateReflex(int v) {
            const bool isReflex = orient(p[prev[v]], p[v], p[next[v]]) <= 0.0;
            if (isReflex && reflexSlot[v] < 0) {
                reflexSlot[v] = (int)reflex.size();
                reflex.push_back(v);
            } else if (!isReflex && reflexSlot[v] >= 0) {
                removeReflex(v);
            }
        }

        void removeReflex(int v) {
            const int slot = reflexSlot[v];
            if (slot < 0) {
                return;
            }
            const int last = reflex.back();
            reflex[slot] = last;
            reflexSlot[last] = slot;
            reflex.pop_back();
            reflexSlot[v] = -1;
        }

        void unlink(int v) {
            removeReflex(v);
            next[prev[v]] = next[v];
            prev[next[v]] = prev[v];
        }

        // The edge v -> next[v] belongs to triangle t.
        void attach(int v, int t) {
            const int d = edgeDiagonal[v];
            if (d >= 0) {
                diagonals[d].second = t;
            }
        }

        // b is strictly convex; no other vertex lies in or on the triangle a, b, c. Only vertices
        // that are not strictly convex can be there.
        bool isEar(int a, int b, int c) const {
            const Point& pa = p[a];
            const Point& pb = p[b];
            const Point& pc = p[c];
            const double minX = std::min(pa.x, std::min(pb.x, pc.x));
            const double maxX = std::max(pa.x, std::max(pb.x, pc.x));
            const double minY = std::min(pa.y, std::min(pb.y, pc.y));
            const double maxY = std::max(pa.y, std::max(pb.y, pc.y));
            for (int r : reflex) {
                if (r == a || r == b || r == c) {
                    continue;
                }
                const Point& q = p[r];
                if (q.x < minX || q.x > maxX || q.y < minY || q.y > maxY) {
                    continue;
                }
                // a vertex at the same place as a or c touches the ear only at that corner
                if (samePoint(q, pa) || samePoint(q, pc)) {
                    continue;
                }
                if (orient(pa, pb, q) >= 0.0 && orient(pb, pc, q) >= 0.0 && orient(pc, pa, q) >= 0.0) {
                    return false;
                }
            }
            return true;
        }

        void clip(int a, int b, int c) {
            const int t = (int)triangles.size();
            triangles.push_back({a, b, c});
            attach(a, t);
            attach(b, t);
            unlink(b);
            edgeDiagonal[a] = (int)diagonals.size();
            diagonals.push_back({a, c, t, -1});
            updateReflex(a);
            updateReflex(c);
        }
    };

    int findRoot(std::vector<int>& parent, int t) {
        int root = t;
        while (parent[root] != root) {
            root = parent[root];
        }
        while (parent[t] != root) {
            const int up = parent[t];
            parent[t] = root;
            t = up;
        }
        return root;
    }

    int indexOf(const std::vector<int>& piece, int v) {
        for (size_t i = 0; i < piece.size(); i++) {
            if (piece[i] == v) {
                return (int)i;
            }
        }
        return -1;
    }

    // Hertel-Mehlhorn: removes diagonals in creation order while both ends stay convex and the
    // merged piece has at most maxVertices vertices. Returns the pieces (vertex indices, CCW)
    // ordered by their first triangle.
    std::vector<std::vector<int>> mergeTriangles(const std::vector<Point>& p, const std::vector<Triangle>& triangles,
                                                 const std::vector<Diagonal>& diagonals, int maxVertices) {
        const int count = (int)triangles.size();
        std::vector<std::vector<int>> pieces(count);
        std::vector<int> parent(count);
        for (int t = 0; t < count; t++) {
            pieces[t] = {triangles[t].a, triangles[t].b, triangles[t].c};
            parent[t] = t;
        }
        std::vector<int> merged;
        merged.reserve(maxVertices);
        for (const Diagonal& d : diagonals) {
            if (d.second < 0) {
                continue;
            }
            const int r1 = findRoot(parent, d.first);
            const int r2 = findRoot(parent, d.second);
            if (r1 == r2) {
                continue;
            }
            const std::vector<int>& p1 = pieces[r1];   // holds c -> a
            const std::vector<int>& p2 = pieces[r2];   // holds a -> c
            const int n1 = (int)p1.size();
            const int n2 = (int)p2.size();
            if (n1 + n2 - 2 > maxVertices) {
                continue;
            }
            const int a1 = indexOf(p1, d.a);
            const int a2 = indexOf(p2, d.a);
            if (a1 < 0 || a2 < 0 || p1[(a1 + n1 - 1) % n1] != d.c || p2[(a2 + 1) % n2] != d.c) {
                continue;
            }
            // merged: p1 from a round to c, then p2 after c up to before a
            const int afterA = p1[(a1 + 1) % n1];
            const int beforeC = p1[(a1 + n1 - 2) % n1];
            const int afterC = p2[(a2 + 2) % n2];
            const int beforeA = p2[(a2 + n2 - 1) % n2];
            if (orient(p[beforeA], p[d.a], p[afterA]) < 0.0 || orient(p[beforeC], p[d.c], p[afterC]) < 0.0) {
                continue;
            }
            merged.clear();
            for (int i = 0; i < n1; i++) {
                merged.push_back(p1[(a1 + i) % n1]);
            }
            for (int i = 2; i < n2; i++) {
                merged.push_back(p2[(a2 + i) % n2]);
            }
            const int root = std::min(r1, r2);
            const int other = std::max(r1, r2);
            pieces[root].swap(merged);
            pieces[other].clear();
            parent[other] = root;
        }
        std::vector<std::vector<int>> result;
        for (int t = 0; t < count; t++) {
            if (parent[t] == t && !pieces[t].empty()) {
                result.push_back(std::move(pieces[t]));
            }
        }
        return result;
    }

    // Fan split of a convex counter-clockwise polygon into pieces of at most maxVertices.
    std::vector<std::vector<int>> splitFan(const std::vector<int>& polygon, int maxVertices) {
        std::vector<std::vector<int>> result;
        const int n = (int)polygon.size();
        int k = 1;
        while (k < n - 1) {
            const int end = std::min(k + maxVertices - 2, n - 1);
            std::vector<int> piece;
            piece.reserve(end - k + 2);
            piece.push_back(polygon[0]);
            for (int i = k; i <= end; i++) {
                piece.push_back(polygon[i]);
            }
            result.push_back(std::move(piece));
            k = end;
        }
        return result;
    }

}

Decomposition2D jasperengine::decomposePolygon2D(const std::vector<Vector2>& points, float weldDistance, int maxVertices) {
    Decomposition2D result;
    if (maxVertices < 3) {
        maxVertices = 3;
    }

    std::vector<Point> input(points.size());
    double minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0;
    for (size_t i = 0; i < points.size(); i++) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
            result.error = "polygon has a non-finite point";
            return result;
        }
        input[i] = {(double)points[i].x, (double)points[i].y};
        if (i == 0) {
            minX = maxX = input[i].x;
            minY = maxY = input[i].y;
        } else {
            minX = std::min(minX, input[i].x);
            maxX = std::max(maxX, input[i].x);
            minY = std::min(minY, input[i].y);
            maxY = std::max(maxY, input[i].y);
        }
    }
    const double scale = std::max(maxX - minX, maxY - minY);
    const double weld = (weldDistance > 0.0f && std::isfinite(weldDistance)) ? (double)weldDistance : 0.0;
    // straight on: closer than the weld distance to the line (a tiny fraction of the size
    // without one); no area: rounding noise only (thin pieces are dropped by step 6)
    const double collinearTolerance = weld > 0.0 ? weld : 1e-9 * scale;
    const double zeroArea = 1e-12 * scale * scale;

    // 1. weld and remove collinear points until nothing changes (a removed spike can bring two
    // close points together)
    std::vector<int> ring(points.size());
    for (size_t i = 0; i < points.size(); i++) {
        ring[i] = (int)i;
    }
    std::vector<int> scratch;
    scratch.reserve(points.size());
    size_t before;
    do {
        before = ring.size();
        weldRing(input, ring, weld * weld, scratch);
        if (ring.size() >= 3) {
            removeCollinear(input, ring, collinearTolerance, scratch);
        }
    } while (ring.size() >= 3 && ring.size() != before);
    if (ring.size() < 3) {
        result.error = "polygon has fewer than 3 distinct points";
        return result;
    }

    // 2. counter-clockwise
    const double signedArea2 = area2(input, ring);
    if (signedArea2 < 0.0) {
        std::reverse(ring.begin(), ring.end());
    }
    const int n = (int)ring.size();
    std::vector<Point> p(n);
    for (int i = 0; i < n; i++) {
        p[i] = input[ring[i]];
    }

    auto toPiece = [&](const std::vector<int>& indices) {
        std::vector<Vector2> piece;
        piece.reserve(indices.size());
        for (int index : indices) {
            piece.push_back(points[ring[index]]);
        }
        return piece;
    };

    std::vector<std::vector<int>> pieces;

    // 3. not simple: the convex hull (a figure-eight can have a zero signed area, so this comes
    // before the area check)
    if (!isSimple(p)) {
        result.selfIntersecting = true;
        const std::vector<int> hull = convexHull(p);
        std::vector<int> hullRing(hull.size());
        for (size_t i = 0; i < hull.size(); i++) {
            hullRing[i] = (int)i;
        }
        std::vector<Point> hullPoints(hull.size());
        for (size_t i = 0; i < hull.size(); i++) {
            hullPoints[i] = p[hull[i]];
        }
        if (hull.size() < 3 || std::fabs(area2(hullPoints, hullRing)) <= zeroArea) {
            result.error = "polygon has zero area";
            return result;
        }
        pieces = splitFan(hull, maxVertices);
    } else {
        if (std::fabs(signedArea2) <= zeroArea) {
            result.error = "polygon has zero area";
            return result;
        }

        bool convex = true;
        for (int i = 0; i < n && convex; i++) {
            convex = orient(p[(i + n - 1) % n], p[i], p[(i + 1) % n]) > 0.0;
        }
        result.convexInput = convex;

        if (convex && n <= maxVertices) {
            // 4. already a piece
            std::vector<int> all(n);
            for (int i = 0; i < n; i++) {
                all[i] = i;
            }
            pieces.push_back(std::move(all));
        } else {
            // 5. ear clipping, then Hertel-Mehlhorn
            EarClipper clipper(p);
            result.droppedPieces += clipper.run();
            pieces = mergeTriangles(p, clipper.triangles, clipper.diagonals, maxVertices);
        }
    }

    // 6. no area, or narrower than the weld distance (Box2D's hull would fail)
    for (const std::vector<int>& piece : pieces) {
        double area = 0.0;
        double longest2 = 0.0;
        const size_t count = piece.size();
        for (size_t i = 0; i < count; i++) {
            const Point& a = p[piece[i]];
            const Point& b = p[piece[(i + 1) % count]];
            longest2 = std::max(longest2, distance2(a, b));
            if (i >= 1 && i + 1 < count) {
                area += orient(p[piece[0]], a, b);
            }
        }
        if (count < 3 || area <= zeroArea || area < weld * std::sqrt(longest2)) {
            result.droppedPieces++;
            continue;
        }
        result.pieces.push_back(toPiece(piece));
    }
    if (result.pieces.empty() && result.error.empty()) {
        result.error = "polygon has no piece wide enough";
    }
    return result;
}
