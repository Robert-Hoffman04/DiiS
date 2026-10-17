/****************************************************************************
 * DiiS - lib_ini.h
 *
 * Flat key=value files (config.ini, game.ini, banner.ini). '#' and ';' start
 * a comment line, [section] lines are ignored, keys are case-insensitive,
 * whitespace around keys and values is trimmed. Order is kept on write.
 ***************************************************************************/
#ifndef DIIS_LIB_INI_H
#define DIIS_LIB_INI_H

#include <string>
#include <vector>

struct LibIni {
	std::vector<std::pair<std::string, std::string> > kv;

	bool load(const char *path);                          // false: file missing/unreadable (kv cleared)
	bool save(const char *path, const char *header) const; // header: one '#' comment line, may be 0
	const std::string *find(const char *key) const;
	std::string get(const char *key, const char *def = "") const;
	void set(const char *key, const std::string &value); // replaces, or appends
	void erase(const char *key);
};

#endif // DIIS_LIB_INI_H
