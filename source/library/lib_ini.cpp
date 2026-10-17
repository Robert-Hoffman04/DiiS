/****************************************************************************
 * DiiS - lib_ini.cpp   (see lib_ini.h)
 ***************************************************************************/
#include "lib_ini.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static std::string trim(const std::string &s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
	return s.substr(a, b - a);
}

bool LibIni::load(const char *path)
{
	kv.clear();
	FILE *f = fopen(path, "rb");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		const std::string l = trim(line);
		if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') continue;
		const size_t eq = l.find('=');
		if (eq == std::string::npos) continue;
		set(trim(l.substr(0, eq)).c_str(), trim(l.substr(eq + 1)));
	}
	fclose(f);
	return true;
}

bool LibIni::save(const char *path, const char *header) const
{
	FILE *f = fopen(path, "wb");
	if (!f) return false;
	if (header) fprintf(f, "# %s\n", header);
	for (size_t i = 0; i < kv.size(); i++)
		fprintf(f, "%s=%s\n", kv[i].first.c_str(), kv[i].second.c_str());
	return fclose(f) == 0;
}

const std::string *LibIni::find(const char *key) const
{
	for (size_t i = 0; i < kv.size(); i++)
		if (!strcasecmp(kv[i].first.c_str(), key)) return &kv[i].second;
	return 0;
}

std::string LibIni::get(const char *key, const char *def) const
{
	const std::string *v = find(key);
	return v ? *v : std::string(def);
}

void LibIni::set(const char *key, const std::string &value)
{
	for (size_t i = 0; i < kv.size(); i++)
		if (!strcasecmp(kv[i].first.c_str(), key)) { kv[i].second = value; return; }
	kv.push_back(std::make_pair(std::string(key), value));
}

void LibIni::erase(const char *key)
{
	for (size_t i = 0; i < kv.size(); i++)
		if (!strcasecmp(kv[i].first.c_str(), key)) { kv.erase(kv.begin() + i); return; }
}
