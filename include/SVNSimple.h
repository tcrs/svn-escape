#ifndef SVNSIMPLE_H__
#define SVNSIMPLE_H__

#include <vector>
#include <string>

extern "C" {
#include <time.h>
#include <svn_types.h>
}

struct svn_error_t;
struct svn_ra_session_t;
struct svn_ra_callbacks2_t;
struct svn_log_entry_t;
struct apr_pool_t;

class SVNSimple
{
public:
	struct Revision
	{
		struct File
		{
			File() : m_action('X'), m_type('U'), m_expand(false) { }
			char m_action;
			char m_type;
			bool m_expand;
			std::string m_relPath;
		};

		svn_revnum_t m_revision;
		std::string m_user;
		std::string m_log;
		time_t m_date;

		std::vector<File> m_files;
	};

	static void Init();
	static void Shutdown();

	SVNSimple(std::string url, std::string username, std::string password);
	~SVNSimple();

	svn_revnum_t GetLatestRevision();
	void Replay(std::vector<Revision>& log, svn_revnum_t from, svn_revnum_t to, bool expandDirectories = true);
	void GetLog(std::vector<Revision>& log, svn_revnum_t from, svn_revnum_t to, bool expandDirectories = true);

	void CatFile(std::string const& relPath, svn_revnum_t revision);
	void CatFile(char const* relPath, svn_revnum_t revision);

protected:
	static svn_error_t* RevisionThunk(void* batonv, svn_log_entry_t* entry, apr_pool_t* basePool);
	void ProcessRevision(Revision& rev, svn_log_entry_t* entry, apr_pool_t* basePool);
	void ExpandDirectories(std::vector<Revision>& log);
	void ExpandDirectory(Revision const& rev, Revision::File& file, std::vector<Revision::File>& extras);
	void AddDirectoryFile(Revision const& rev, Revision::File& file, std::vector<Revision::File>& extras, char const* name);

	apr_pool_t* m_pool;
	svn_ra_callbacks2_t* m_callbacks;
	svn_ra_session_t* m_session;

	std::string m_subtree;
};

#endif

