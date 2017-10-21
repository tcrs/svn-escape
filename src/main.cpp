#include "SVNSimple.h"
#include "FastExport.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fnmatch.h>
#include <fcntl.h>

#include <string>
#include <vector>
#include <map>
#include <sstream>

#define LF "\x0A"

enum Configs
{
	Config_RepoURL,
	Config_RepoName,
	Config_GitRef,
	Config_ParentSHA,
	Config_StartRev,
	Config_EndRev,
	Config_IgnorePath,
	Config_Username,
	Config_Password,
	Config_UserPrefix,

	Config_NUM
};

typedef std::map<std::string, std::string> UserMap;
struct Config
{
	struct Item
	{
		char const* name;
		unsigned int len;
		char const* help;
	};
	static Item const keys[Config_NUM];

	std::vector<std::string> ignoredPaths;
	std::string config[Config_NUM];

	UserMap users;
};

#define DefItem(str, help) { str, (sizeof(str) / sizeof(str[0])) - 1, help }
Config::Item const Config::keys[Config_NUM] = {
	DefItem("repo-url ", "The URL of the svn repository to fetch from"),
	DefItem("repo-name ", "The friendly name for this repository"),
	DefItem("git-ref ", "The (fully specified) git ref to update.  Defaults to refs/remotes/svn/repo-name"),
	DefItem("parent-sha ", "The SHA of the object which should be the parent of the first commit fetched.  If not specified then the first revision will not have a parent."),
	DefItem("start-rev ", "The first revision to import"),
	DefItem("end-rev ", "The last revision to import. Default to the youngest revision in the repo."),
	DefItem("ignore-path ", "Ignores any paths which fnmatch against the provided pattern.  Note that all paths are relative to the repo-url. Can be specified multiple times."),
	DefItem("username ", "The username to use when authenticating with the repository"),
	DefItem("password ", "The password to use when authenticating with the repository"),
	DefItem("remove-user-prefix ", "A prefix which will be removed from usernames with that prefix"),
};
#undef DefItem

template<typename T>
static T Min(T const& a, T const& b) {
	return a < b? a : b;
}

static std::vector<std::string>::const_iterator Matches(std::string const& str, std::vector<std::string> const& patterns)
{
	for(std::vector<std::string>::const_iterator pit = patterns.begin(); pit != patterns.end(); ++pit) {
		if(fnmatch(pit->c_str(), str.c_str(), 0) == 0)
		{
			return pit;
		}
	}

	return patterns.end();
}

void FilterIgnoredFiles(std::vector<SVNSimple::Revision>& revisions, std::vector<std::string> const& ignorePatterns)
{
	std::vector<std::string>::const_iterator pattern;
	for(std::vector<SVNSimple::Revision>::iterator rit = revisions.begin(); rit != revisions.end(); ++rit) {
		for(std::vector<SVNSimple::Revision::File>::iterator fit = rit->m_files.begin(); fit != rit->m_files.end(); ++fit) {
			SVNSimple::Revision::File& file = *fit;
			pattern = Matches(file.m_relPath, ignorePatterns);
			if(pattern != ignorePatterns.end()) {
				file.m_action = 'I';
				printf("# %lu > %c %s - Ignored by ignore pattern %s" LF, rit->m_revision, file.m_action, file.m_relPath.c_str(), pattern->c_str());
			}
		}
	}
}

void AddSVNSourceTag(std::vector<SVNSimple::Revision>& revisions, std::string const& repoName)
{
	std::string tag("\n\nsvn-source: ");
	tag.append(repoName);
	tag.append("@");

	for(std::vector<SVNSimple::Revision>::iterator rit = revisions.begin(); rit != revisions.end(); ++rit) {
		std::ostringstream ss;
		ss << rit->m_log;
		ss << tag;
		ss << rit->m_revision;

		rit->m_log = ss.str();
	}
}

void RewriteCommitters(std::vector<SVNSimple::Revision>& revisions, UserMap users, std::string const& prefix)
{
	for(std::vector<SVNSimple::Revision>::iterator rit = revisions.begin(); rit != revisions.end(); ++rit) {
		SVNSimple::Revision& rev = *rit;

		if(rev.m_user.size()) {
			if(prefix.size() && rev.m_user.size() >= prefix.size()) {
				if(rev.m_user.compare(0, prefix.size(), prefix) == 0)
				{
					rev.m_user = rev.m_user.substr(prefix.size());
				}
			}
			UserMap::const_iterator it = users.find(rev.m_user);
			if(it == users.end()) {
				std::ostringstream ss;
				ss << rev.m_user << " <" << rev.m_user << "@localhost>";
				rev.m_user = ss.str();
			} else {
				rev.m_user = it->second;
			}
		} else {
			rev.m_user = "Unknown <Unknown@localhost>";
		}
	}
}

void ReadConfigLine(Config& config, char const* line)
{
	for(unsigned int i = 0; i < Config_NUM; i += 1)
	{
		if(strncmp(line, Config::keys[i].name, Config::keys[i].len) == 0)
		{
			if(i == Config_IgnorePath)
			{
				config.ignoredPaths.push_back(line + Config::keys[i].len);
			}
			else
			{
				config.config[i] = line + Config::keys[i].len;
			}
			break;
		}
	}
}

void ReadUserLine(Config& config, char const* line)
{
	char const* equals = strchr(line, '=');
	if(equals == NULL)
	{
		fprintf(stderr, "[ERROR] Malformed user line: %s\n", line);
		return;
	}

	char const* endKey = equals - 1;
	while(endKey[0] == ' ' && endKey != line) endKey -= 1;

	char const* startValue = equals + 1;
	while(startValue[0] == ' ') startValue += 1;

	if(endKey == line || startValue[0] == '\0')
	{
		fprintf(stderr, "[ERROR] Malformed user line: %s\n", line);
		return;
	}

	config.users[std::string(line, endKey - line + 1)] = startValue;
}

void ReadConfig(Config& config)
{
	static size_t const c_bufSz = 2048;
	static size_t const c_lineSz = 512;

	char buf[c_bufSz];
	char line[c_lineSz];
	unsigned int linePos = 0;
	size_t len;

	while((len = fread(buf, 1, c_bufSz, stdin)))
	{
		for(size_t i = 0; i < len; i += 1)
		{
			if(buf[i] == '\n' || buf[i] == '\r')
			{
				if(linePos == 0) continue; // Skip empty lines

				line[linePos] = '\0';

				if(line[0] == '=')
				{
					ReadConfigLine(config, line + 1);
				}
				else if(line[0] == '+')
				{
					ReadUserLine(config, line + 1);
				}
				else
				{
					fprintf(stderr, "[ERROR] Unknown config line type: %s\n", line);
				}
				linePos = 0;
			}
			else
			{
				line[linePos++] = buf[i];
				if(linePos == c_lineSz)
				{
					fprintf(stderr, "[ERROR] Line too long in input config\n");
					linePos = 0;
				}
			}
		}
	}
}

static void help()
{
	fprintf(stderr,
		"Pipe a configuration into me!\n"
		"Configuration options start with an =, user name mappings start with a +\n"
		"example for a non-incremental import:\n\n"

		"=repo-url svn://localhost/bar\n"
		"=repo-name Bar\n"
		"=ignore-path ignore/*\n"
		"=start-rev 0\n"
		"+Harry = Harry Smith <hsmith@aplace.com>\n"
		"\n"
		"All config options:\n"
	);
	for(unsigned int i = 0; i < Config_NUM; i += 1)
	{
		fprintf(stderr, "%s:  %s\n", Config::keys[i].name, Config::keys[i].help);
	}
}

void Export(Config& config)
{
	SVNSimple connection(config.config[Config_RepoURL], config.config[Config_Username], config.config[Config_Password]);

	svn_revnum_t startRev = strtoul(config.config[Config_StartRev].c_str(), NULL, 0);
	svn_revnum_t latestRev = connection.GetLatestRevision();
	svn_revnum_t endRev = latestRev;
	if(config.config[Config_EndRev].size())
	{
		endRev = strtoul(config.config[Config_EndRev].c_str(), NULL, 0);
		endRev = Min(endRev, latestRev);
	}

	if(endRev < startRev) {
		printf(
			"progress No more revisions available from %s (%s)" LF,
			config.config[Config_RepoURL].c_str(),
			config.config[Config_RepoName].c_str()
		);
		return;
	}

	{
		std::string const& parentSHA = config.config[Config_ParentSHA];
		printf(
			"progress Starting import from %s (%s) %lu:%lu to %s (%s)" LF,
			config.config[Config_RepoURL].c_str(),
			config.config[Config_RepoName].c_str(),
			startRev,
			endRev,
			config.config[Config_GitRef].c_str(),
			parentSHA.size()? parentSHA.c_str() : "Unparented"
		);
	}

	FastExport exporter(config.config[Config_GitRef], config.config[Config_ParentSHA]);

	std::vector<SVNSimple::Revision> revisions;
	svn_revnum_t curStart = startRev;
	svn_revnum_t curEnd = Min(endRev, curStart + 256);
	do
	{
		revisions.clear();
		printf("progress Getting log for revisions %lu:%lu" LF, curStart, curEnd);
		connection.Replay(revisions, curStart, curEnd);

		FilterIgnoredFiles(revisions, config.ignoredPaths);
		AddSVNSourceTag(revisions, config.config[Config_RepoName]);
		RewriteCommitters(revisions, config.users, config.config[Config_UserPrefix]);

		exporter.DumpRevisions(connection, revisions);

		curStart = curEnd + 1;
		curEnd = Min(endRev, curStart + 256);
	}
	while(curStart <= endRev && curEnd <= endRev);
}

int main(int argc, char** argv)
{
	if(argc > 1)
	{
		help();
		return -1;
	}

	Config config;

	SVNSimple::Init();

	ReadConfig(config);
	if(config.config[Config_RepoURL].size() == 0)
	{
		fprintf(stderr, "[ERROR] No repo url specified\n");
		help();
		return -1;
	}
	if(config.config[Config_RepoName].size() == 0)
	{
		fprintf(stderr, "[ERROR] No repo name specified\n");
		help();
		return -1;
	}
	if(config.config[Config_StartRev].size() == 0)
	{
		fprintf(stderr, "[ERROR] No start revision specified\n");
		help();
		return -1;
	}
	if(config.config[Config_GitRef].size() == 0)
	{
		config.config[Config_GitRef] = "refs/remotes/svn/";
		config.config[Config_GitRef].append(config.config[Config_RepoName]);
	}

	for(unsigned int i = 0; i < Config_NUM; i += 1)
	{
		if(config.config[i].size())
		{
			printf("# Config: \"%s\" = \"%s\"" LF, Config::keys[i].name, config.config[i].c_str());
		}
	}

	Export(config);

	SVNSimple::Shutdown();

	return 0;
}
