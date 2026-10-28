/*
 * lod_survey.cpp - prints every model's level-of-detail numbers (see
 * docs/level-of-detail.md): scale, radius, the "drawn as a dot" and "not
 * drawn" distance exponents (+$14, +$10), and the distance tests ($0b/$0c
 * "if z") in its bytecode. Build from the repo root:
 *   g++ -std=c++17 -O1 -w -Itools/fe2ShipBuilder -Ife2 \
 *       tools/fe2ShipBuilder/lod_survey.cpp tools/fe2ShipBuilder/fe2core.cpp -o lod_survey
 */
#include "fe2core.h"
#include <cstdio>
using namespace fe2;

int main()
{
	const Image &img = Image::game();
	for (int n : img.model_numbers())
	{
		auto m = img.model(n);
		if (!m)
			continue;
		std::vector<Instr> code = decode(img.data, m->code_addr());
		int z = 0, nrm = 0, dist = 0, planet = 0, sub = 0;
		std::string zs, ds;
		for (auto &i : code)
		{
			char b[32];
			if ((i.op == 0x0b || i.op == 0x0c) && i.w.size() > 1)
			{
				if (i.w[1] & 0x8000)
					nrm++;
				else
				{
					z++;
					snprintf(b, sizeof b, "%s%d ", i.op == 0x0b ? ">" : "<", i.w[1]);
					zs += b;
				}
			}
			if (i.op == 0x17 && i.w.size() > 1)
			{
				dist++;
				snprintf(b, sizeof b, "%s%d ", (i.w[1] & 0x8000) ? "<" : ">", i.w[1] & 0x7fff);
				ds += b;
			}
			planet += i.op == 0x1f;
			sub += i.op == 0x0e || i.op == 0x1b;
		}
		printf("%3d sc=%d sc2=%d rad=%d maxexp=%d dotcol=%04x dotexp=%d | z:%d [%s] dist:%d [%s] sub:%d planet:%d %s\n",
			   n, m->scale, (int16_t)m->scale2, m->radius, (int16_t)m->x10, m->colour, (int16_t)m->x14, z,
			   zs.c_str(), dist, ds.c_str(), sub, planet, m->ship_name().c_str());
	}
}
