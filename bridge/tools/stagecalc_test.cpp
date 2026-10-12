// bridge #47: the virtual region's box for every campaign map (stagecalc.hpp, no game needed).
//   g++ -std=c++17 -O1 -I../src/bridge -o /tmp/stagecalc_test stagecalc_test.cpp && /tmp/stagecalc_test "<Portal 2>/portal2/maps"
#include "stagecalc.hpp"

#include <dirent.h>
#include <algorithm>
#include <cstdlib>

int main(int argc, char **argv) {
	const char *p2 = std::getenv("PORTAL2_DIR");  // tools/portal2_dir.sh finds it
	if (argc < 2 && !(p2 && *p2)) return std::fprintf(stderr, "usage: stagecalc_test <Portal 2>/portal2/maps   (or set PORTAL2_DIR; see tools/portal2_dir.sh)\n"), 2;
	const std::string dir = argc > 1 ? argv[1] : std::string(p2) + "/portal2/maps";
	std::vector<std::string> maps;
	if (DIR *d = opendir(dir.c_str())) {
		while (dirent *e = readdir(d)) {
			const std::string n = e->d_name;
			if (n.size() > 4 && n.substr(n.size() - 4) == ".bsp" && (n.rfind("sp_", 0) == 0 || n.rfind("mp_", 0) == 0)) maps.push_back(n);
		}
		closedir(d);
	}
	std::sort(maps.begin(), maps.end());
	const float half[3] = {3200, 3200, 2400};
	int bad = 0;
	float worst = 1e9f;
	for (const std::string &m : maps) {
		stagecalc::Box b;
		if (!stagecalc::BspExtent(dir + "/" + m, b)) {
			std::printf("%-34s unreadable\n", m.c_str());
			++bad;
			continue;
		}
		float c[3], gap;
		stagecalc::ChooseRegion(b, half, 16300.0f, c, gap);
		worst = std::fmin(worst, gap);
		std::printf("%-34s extent x %6.0f..%6.0f y %6.0f..%6.0f z %6.0f..%6.0f  region centre (%6.0f %6.0f %6.0f) gap %6.0f%s\n", m.c_str(), b.mn[0], b.mx[0], b.mn[1], b.mx[1], b.mn[2], b.mx[2], c[0], c[1],
		            c[2], gap, gap < 1500 ? "  NO ROOM" : "");
		if (gap < 1500) ++bad;
	}
	std::printf("%zu maps, worst gap %.0f, %d without room or unreadable\n", maps.size(), worst, bad);
	return bad ? 1 : 0;
}
