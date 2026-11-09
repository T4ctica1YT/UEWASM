#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

struct Shape {
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::string name() const = 0;
};

struct Circle : Shape {
    double r;
    explicit Circle(double r) : r(r) {}
    double area() const override { return 3.14159265358979 * r * r; }
    std::string name() const override { return "circle"; }
};

struct Rect : Shape {
    double w, h;
    Rect(double w, double h) : w(w), h(h) {}
    double area() const override { return w * h; }
    std::string name() const override { return "rect"; }
};

static int destroyed = 0;
struct Tracker {
    int id;
    explicit Tracker(int i) : id(i) {}
    ~Tracker() { destroyed += id; }
};

int main()
{
    std::vector<std::unique_ptr<Shape>> shapes;
    for (int i = 1; i <= 6; i++) {
        if (i % 2) shapes.push_back(std::make_unique<Circle>(i * 0.5));
        else shapes.push_back(std::make_unique<Rect>(i, i + 1.5));
    }
    std::sort(shapes.begin(), shapes.end(), [](auto& a, auto& b) { return a->area() < b->area(); });
    for (auto& s : shapes) std::cout << s->name() << " " << std::fixed << std::setprecision(3) << s->area() << "\n";

    std::map<std::string, int> counts;
    std::string text = "the quick brown fox jumps over the lazy dog the end";
    std::istringstream in(text);
    for (std::string w; in >> w;) counts[w]++;
    for (auto& [k, v] : counts) std::cout << k << "=" << v << ";";
    std::cout << "\n";

    std::unordered_map<int, std::string> um;
    for (int i = 0; i < 200; i++) um[i * 7919 % 1009] = std::to_string(i);
    long sum = 0;
    for (auto& [k, v] : um) sum += k * (long)std::stoi(v);
    std::cout << "um size " << um.size() << " sum " << sum << "\n";

    std::vector<int> v(50);
    std::iota(v.begin(), v.end(), -20);
    std::function<int(int)> f = [&](int x) { return x * x - 3 * x; };
    std::transform(v.begin(), v.end(), v.begin(), f);
    std::cout << "acc " << std::accumulate(v.begin(), v.end(), 0LL) << " max " << *std::max_element(v.begin(), v.end()) << "\n";

    std::string s;
    for (int i = 0; i < 40; i++) s += char('a' + i % 26);
    s.replace(5, 10, "XYZ");
    std::reverse(s.begin(), s.end());
    std::cout << s << " find=" << s.find("ZYX") << " sub=" << s.substr(3, 7) << "\n";

    {
        std::vector<Tracker> ts;
        for (int i = 1; i <= 10; i++) ts.emplace_back(i);
    }
    std::ostringstream os;
    os << std::hex << std::uppercase << 0xC0FFEE << " " << std::dec << std::setw(8) << std::setfill('.') << 42 << " " << std::scientific << 12345.678;
    std::cout << os.str() << " destroyed=" << destroyed << std::endl;
    printf("printf still works %d %.2f\n", 7, std::sqrt(2.0));
    return 0;
}
