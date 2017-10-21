#include "SVNSimple.h"
#include "Exception.h"

extern "C" {
#include <apr_lib.h>
#include <apr_getopt.h>
#include <apr_general.h>

#include <svn_ra.h>
#include <svn_types.h>
#include <svn_dirent_uri.h>
#include <svn_pools.h>
#include <svn_auth.h>
#include <svn_cmdline.h>
#include <svn_io.h>
}

#define LF "\x0A"

#define ACTUALLY_GET_FILE_DATA (1)

static svn_error_t* cancel_func(void* baton) { return NULL; }

/* This function is from svn-fast-export.c
 * Author: Chris Lee <clee@kde.org>
 * License: MIT <http://www.opensource.org/licenses/mit-license.php>
 */
static time_t ParseDate(char const* svn_date, unsigned int len)
{
	struct tm tm = {0};
	char *date = new char[len];
	strncpy(date, svn_date, len - 8);
	strptime(date, "%Y-%m-%dT%H:%M:%S", &tm);
	delete[] date;
	return mktime(&tm);
}

void SVNSimple::Init()
{
	if(apr_initialize() != APR_SUCCESS) {
		throw EXCEPTION(("APR Initialisation failed"));
	}
}

void SVNSimple::Shutdown()
{
	apr_terminate();
}

SVNSimple::SVNSimple(std::string url, std::string username, std::string password)
{
	svn_error_t* err;
	apr_hash_t* config;

	m_pool = svn_pool_create(NULL);
	if((err = svn_ra_create_callbacks(&m_callbacks, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	if((err = svn_config_get_config(&config, NULL, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}
	{
		svn_config_t* cfg = static_cast<svn_config_t*>(apr_hash_get(config, SVN_CONFIG_CATEGORY_CONFIG, APR_HASH_KEY_STRING));
		svn_cmdline_create_auth_baton(&m_callbacks->auth_baton, 0, username.c_str(), password.c_str(), NULL, 0, 1, cfg, cancel_func, NULL, m_pool);
	}

	if((err = svn_ra_open4(&m_session, NULL, url.c_str(), NULL, m_callbacks, NULL, config, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	char const* sessionURL;
	char const* rootURL;
	if((err = svn_ra_get_session_url(m_session, &sessionURL, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}
	if((err = svn_ra_get_repos_root2(m_session, &rootURL, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	m_subtree.append(sessionURL + strlen(rootURL));
}

SVNSimple::~SVNSimple()
{
	svn_pool_destroy(m_pool);
}

svn_revnum_t SVNSimple::GetLatestRevision()
{
	svn_revnum_t rev;
	svn_error_t* err;
	if((err = svn_ra_get_latest_revnum(m_session, &rev, m_pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}
	return rev;
}

void SVNSimple::CatFile(std::string const &relPath, svn_revnum_t revision)
{
	CatFile(relPath.c_str(), revision);
}

void SVNSimple::CatFile(char const* relPath, svn_revnum_t revision)
{
#if ACTUALLY_GET_FILE_DATA
	apr_pool_t* pool = svn_pool_create(m_pool);
	svn_error_t* err;

	svn_dirent_t* ent;
	if((err = svn_ra_stat(m_session, relPath, revision, &ent, pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	if(ent == NULL) {
		throw EXCEPTION(("Could not get dirent for file %s at revision %lu", relPath, revision));
	}

	if(ent->kind != svn_node_file) {
		throw EXCEPTION(("Entry %s at revision %lu is not a file", relPath, revision));
	}

	svn_stream_t* stream = svn_stream_create(NULL, pool);
	if((err = svn_stream_for_stdout(&stream, pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	printf("data %lu" LF, ent->size);
	fflush(stdout);
	if((err = svn_ra_get_file(m_session, relPath, revision, stream, NULL, NULL /* TODO: props*/, pool))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}
	fflush(stdout);
	printf(LF);

	svn_pool_destroy(pool);
#else
	printf("data 0" LF);
	printf(LF);
#endif
}

struct RevThunkBaton
{
	RevThunkBaton(SVNSimple& conn, std::vector<SVNSimple::Revision>& rev) :
		m_connection(conn), m_revisions(rev) { }

	SVNSimple& m_connection;
	std::vector<SVNSimple::Revision>& m_revisions;
};

svn_error_t* SVNSimple::RevisionThunk(void* batonv, svn_log_entry_t* entry, apr_pool_t* basePool)
{
	RevThunkBaton* baton = static_cast<RevThunkBaton*>(batonv);
	SVNSimple::Revision rev;
	baton->m_connection.ProcessRevision(rev, entry, basePool);
	baton->m_revisions.push_back(rev);

	return NULL;
}

static void ReadRevProps(SVNSimple::Revision& rev, apr_hash_t* props)
{
		svn_string_t* author = static_cast<svn_string_t*>(apr_hash_get(props, "svn:author", APR_HASH_KEY_STRING));
		svn_string_t* log = static_cast<svn_string_t*>(apr_hash_get(props, "svn:log", APR_HASH_KEY_STRING));
		svn_string_t* date = static_cast<svn_string_t*>(apr_hash_get(props, "svn:date", APR_HASH_KEY_STRING));

		if(author) { rev.m_user = std::string(author->data, author->len); }
		if(log) { rev.m_log = std::string(log->data, log->len); }
		if(date) { rev.m_date = ParseDate(date->data, date->len); }
}

void SVNSimple::ProcessRevision(Revision& rev, svn_log_entry_t* entry, apr_pool_t* basePool)
{
	apr_pool_t* pool = svn_pool_create(basePool);

	rev.m_revision = entry->revision;

	if(entry->revprops)
	{
		ReadRevProps(rev, entry->revprops);
	}

	if(entry->changed_paths2)
	{
		for(apr_hash_index_t* index = apr_hash_first(pool, entry->changed_paths2); index; index = apr_hash_next(index))
		{
			void const* key;
			apr_ssize_t keyLen;
			void* val;
			apr_hash_this(index, &key, &keyLen, &val);

			char const* path = static_cast<char const*>(key);
			svn_log_changed_path2_t* info = static_cast<svn_log_changed_path2_t*>(val);

			printf("# %lu > %c %s" LF, entry->revision, info->action, path);

			Revision::File file;
			file.m_action = info->action;
			switch(info->node_kind)
			{
				case svn_node_file:
					file.m_type = 'F';
					break;
				case svn_node_dir:
					file.m_type = 'D';
					break;
				default:
					file.m_type = 'U';
			}

			if(strncmp(m_subtree.c_str(), path, m_subtree.size())) {
				continue;
			}

			unsigned int nudge = 0;
			if(path[m_subtree.size()] == '/') {
				nudge = 1;
			}
			file.m_relPath = std::string(path + m_subtree.size() + nudge);

			if(info->copyfrom_path) {
				file.m_expand = true;
			}

			rev.m_files.push_back(file);
		}
	}

	svn_pool_destroy(pool);
}

template<typename IterType>
static IterType FindMatching(
	std::string const& needle,
	IterType begin,
	IterType end
)
{
	for(IterType it = begin; it != end; ++it) {
		if(it->m_relPath == needle) {
			return it;
		}
	}

	return end;
}

void SVNSimple::AddDirectoryFile(Revision const& rev, Revision::File& parent, std::vector<Revision::File>& extras, char const* name)
{
	Revision::File subFile(parent);
	subFile.m_type = 'F';
	subFile.m_action = 'A';
	subFile.m_relPath.append("/");
	subFile.m_relPath.append(name);

	{
		std::vector<SVNSimple::Revision::File>::const_iterator pos = FindMatching(subFile.m_relPath, rev.m_files.begin(), rev.m_files.end());
		if(pos != rev.m_files.end()) {
			printf("# %lu > NOEXPAND: %s: Node already in revision (%c, %c)" LF, rev.m_revision, subFile.m_relPath.c_str(), pos->m_type, pos->m_action);
			return;
		}
	}
	{
		std::vector<SVNSimple::Revision::File>::iterator pos = FindMatching(subFile.m_relPath, extras.begin(), extras.end());
		if(pos != extras.end()) {
			printf("# %lu > NOEXPAND: %s: Node already expanded (%c, %c)" LF, rev.m_revision, subFile.m_relPath.c_str(), pos->m_type, pos->m_action);
			return;
		}
	}

	extras.push_back(subFile);
	printf("# %lu > EXPAND %s: %s" LF, rev.m_revision, parent.m_relPath.c_str(), subFile.m_relPath.c_str());
}

void SVNSimple::ExpandDirectory(Revision const& rev, Revision::File& parent, std::vector<Revision::File>& extras)
{
	svn_error_t* err;
	apr_pool_t* pool = svn_pool_create(m_pool);
	apr_hash_t* dirents;

	if((err = svn_ra_get_dir2(
		m_session,
		&dirents,
		NULL,
		NULL,
		parent.m_relPath.c_str(),
		rev.m_revision,
		SVN_DIRENT_KIND, // Only want to know the type of dirents
		pool
	))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	for(apr_hash_index_t* index = apr_hash_first(pool, dirents); index; index = apr_hash_next(index))
	{
		void const* key;
		apr_ssize_t keyLen;
		void* val;
		apr_hash_this(index, &key, &keyLen, &val);

		char const* path = static_cast<char const*>(key);
		svn_dirent_t* info = static_cast<svn_dirent_t*>(val);

		printf("# %s -> %s" LF, parent.m_relPath.c_str(), path);

		switch(info->kind)
		{
			case svn_node_file:
				AddDirectoryFile(rev, parent, extras, path);
				break;
			case svn_node_dir:
				{
					Revision::File subDir(parent);
					subDir.m_relPath.append("/");
					subDir.m_relPath.append(path);

					ExpandDirectory(rev, subDir, extras);
				}
				break;
			default:
				ERROR(("Unknown file kind: \"%s\" in directory \"%s\"", path, parent.m_relPath.c_str()));
		}
	}

	svn_pool_destroy(pool);
}

void SVNSimple::ExpandDirectories(std::vector<Revision>& log)
{
	for(std::vector<Revision>::iterator rit = log.begin(); rit != log.end(); ++rit) {
		Revision& rev = *rit;

		std::vector<Revision::File> expanded;
		for(std::vector<Revision::File>::iterator fit = rit->m_files.begin(); fit != rit->m_files.end(); ++fit) {
			if(fit->m_action == 'R') {
				// Replace: delete the destination before copying into it.
				Revision::File del(*fit);
				del.m_action = 'D';
				expanded.push_back(del);
			}
			expanded.push_back(*fit);

			// Don't expand deletes as they are always recursive (also SVN won't
			// have a dirent for files in a deleted directory).
			if(fit->m_expand && fit->m_type == 'D' && fit->m_action != 'D') {
				ExpandDirectory(rev, *fit, expanded);
			}
		}

		rev.m_files.swap(expanded);
	}
}

void SVNSimple::GetLog(std::vector<Revision>& log, svn_revnum_t from, svn_revnum_t to, bool expandDirectories)
{
	svn_error_t* err;
	apr_pool_t* pool = svn_pool_create(m_pool);
	apr_array_header_t* paths = NULL;

	// Providing a paths argument of an empty string will limit the revisions
	// returned to just those which affect the current path of the session.
	if(m_subtree.size())
	{
		paths = apr_array_make(pool, 1, sizeof(char const*));
		char const** path = static_cast<char const**>(apr_array_push(paths));
		*path = "";
	}

	RevThunkBaton baton(*this, log);

	if((err = svn_ra_get_log2(
		m_session,
		paths,
		from,
		to,
		0, // No limit
		1, // Generate a list of changed paths
		0, // Don't follow copies
		0, // No merge info
		NULL, // TODO: Filter revprops
		&RevisionThunk,
		static_cast<void*>(&baton),
		pool
	))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

	if(expandDirectories) {
		ExpandDirectories(log);
	}

	svn_pool_destroy(pool);
}

#define VERBOSE_REPLAY (0)

static bool MakeRelativePath(std::string& result, char const* path, std::string const& subtree)
{
	if(strncmp(subtree.c_str(), path, subtree.size())) {
		return false;
	}

	unsigned int nudge = 0;
	if(path[subtree.size()] == '/') {
		nudge = 1;
	}
	result = std::string(path + subtree.size() + nudge);

	return true;
}

struct ReplayBaton {
	std::vector<SVNSimple::Revision>* m_log;
	std::string* m_subtree;
};

struct EditBaton {
	SVNSimple::Revision m_rev;
	std::string* m_subtree;
};

static void AddEntry(char action, char type, char const* path, void* batonData)
{
	EditBaton* baton = static_cast<EditBaton*>(batonData);

	SVNSimple::Revision::File file;
	file.m_action = action;
	file.m_type = type;
	if(type == 'D' && action == 'C')
	{
		file.m_expand = true;
	}
	if(MakeRelativePath(file.m_relPath, path, *baton->m_subtree))
	{
		baton->m_rev.m_files.push_back(file);
	}
#if VERBOSE_REPLAY
	else
	{
		fprintf(stderr, "Rejected \"%s\" for subtree \"%s\"\n", path, baton->m_subtree->c_str());
	}
#endif
}

static svn_error_t* set_target_revision(void *edit_baton, svn_revnum_t target_revision, apr_pool_t *scratch_pool)
{
	// Set the target revision for this edit to target_revision.
#if VERBOSE_REPLAY
	fprintf(stderr, "set_target_revision(%p, %lu)\n", edit_baton, target_revision);
#endif
	return SVN_NO_ERROR;
}

static svn_error_t* open_root(void *edit_baton, svn_revnum_t base_revision, apr_pool_t *result_pool, void **root_baton)
{
	// Set *root_baton to a baton for the top directory of the change.
	*root_baton = edit_baton;
#if VERBOSE_REPLAY
	fprintf(stderr, "open_root(%p, %lu) => %p\n", edit_baton, base_revision, *root_baton);
#endif

	return SVN_NO_ERROR;
}

static svn_error_t* delete_entry(const char *path, svn_revnum_t revision, void *parent_baton, apr_pool_t *scratch_pool)
{
	// Remove the directory entry named path, a child of the directory represented by parent_baton.
#if VERBOSE_REPLAY
	fprintf(stderr, "delete_entry(\"%s\", %lu, %p)\n", path, revision, parent_baton);
#endif

	AddEntry('D', 'E', path, parent_baton);

	return SVN_NO_ERROR;
}

svn_error_t* add_directory(const char *path, void *parent_baton, const char *copyfrom_path, svn_revnum_t copyfrom_revision, apr_pool_t *result_pool, void **child_baton)
{
	// We are going to add a new subdirectory named path.
	*child_baton = parent_baton;
#if VERBOSE_REPLAY
	fprintf(stderr, "add_directory(\"%s\", %p) => %p\n", path, parent_baton, *child_baton);
#endif

	if(copyfrom_path)
	{
		AddEntry('C', 'D', path, parent_baton);
	}

	return SVN_NO_ERROR;
}

static svn_error_t* open_directory(const char *path, void *parent_baton, svn_revnum_t base_revision, apr_pool_t *result_pool, void **child_baton)
{
	// We are going to make changes in a subdirectory (of the directory identified by parent_baton).
	*child_baton = parent_baton;
#if VERBOSE_REPLAY
	fprintf(stderr, "open_directory(\"%s\", %p, %lu) => %p\n", path, parent_baton, base_revision, *child_baton);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* change_dir_prop(void *dir_baton, const char *name, const svn_string_t *value, apr_pool_t *scratch_pool)
{
	//Change the value of a directory's property.
#if VERBOSE_REPLAY
	fprintf(stderr, "change_dir_prop(%p, \"%s\")\n", dir_baton, name);
#endif
	return SVN_NO_ERROR;
}

static svn_error_t* close_directory(void *dir_baton, apr_pool_t *scratch_pool)
{
	//We are done processing a subdirectory, whose baton is dir_baton (set by add_directory or open_directory).
#if VERBOSE_REPLAY
	fprintf(stderr, "close_directory(%p)\n", dir_baton);
#endif
	return SVN_NO_ERROR;
}

static svn_error_t* absent_directory(const char *path, void *parent_baton, apr_pool_t *scratch_pool)
{
	//In the directory represented by parent_baton, indicate that path is present as a subdirectory in the edit source, but cannot be conveyed to the edit consumer (perhaps because of authorization restrictions).
#if VERBOSE_REPLAY
	fprintf(stderr, "absent_directory(\"%s\", %p)\n", path, parent_baton);
#endif
	return SVN_NO_ERROR;
}

static svn_error_t* add_file(const char *path, void *parent_baton, const char *copyfrom_path, svn_revnum_t copyfrom_revision, apr_pool_t *result_pool, void **file_baton)
{
	//We are going to add a new file named path.
	*file_baton = parent_baton;
#if VERBOSE_REPLAY
	fprintf(stderr, "add_file(\"%s\", %p) => %p\n", path, parent_baton, *file_baton);
#endif

	AddEntry('A', 'F', path, parent_baton);

	return SVN_NO_ERROR;
}

static svn_error_t* open_file(const char *path, void *parent_baton, svn_revnum_t base_revision, apr_pool_t *result_pool, void **file_baton)
{
	//We are going to make change to a file named path, which resides in the directory identified by parent_baton.
	*file_baton = parent_baton;
#if VERBOSE_REPLAY
	fprintf(stderr, "open_file(\"%s\", %p, %lu) => %p\n", path, parent_baton, base_revision, *file_baton);
#endif

	AddEntry('M', 'F', path, parent_baton);

	return SVN_NO_ERROR;
}

static svn_error_t* apply_textdelta(void *file_baton, const char *base_checksum, apr_pool_t *result_pool, svn_txdelta_window_handler_t *handler, void **handler_baton)
{
	//Apply a text delta, yielding the new revision of a file.
	*handler_baton = file_baton;
	*handler = svn_delta_noop_window_handler;
#if VERBOSE_REPLAY
	fprintf(stderr, "apply_textdelta(%p) => %p\n", file_baton, *handler_baton);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* change_file_prop(void *file_baton, const char *name, const svn_string_t *value, apr_pool_t *scratch_pool)
{
	//Change the value of a file's property.
#if VERBOSE_REPLAY
	fprintf(stderr, "change_file_prop(%p, \"%s\")\n", file_baton, name);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* close_file(void *file_baton, const char *text_checksum, apr_pool_t *scratch_pool)
{
	//We are done processing a file, whose baton is file_baton (set by add_file or open_file).
#if VERBOSE_REPLAY
	fprintf(stderr, "close_file(%p)\n", file_baton);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* absent_file(const char *path, void *parent_baton, apr_pool_t *scratch_pool)
{
	//In the directory represented by parent_baton, indicate that path is present as a file in the edit source, but cannot be conveyed to the edit consumer (perhaps because of authorization restrictions).
#if VERBOSE_REPLAY
	fprintf(stderr, "absent_file(\"%s\", %p)\n", path, parent_baton);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* close_edit(void *edit_baton, apr_pool_t *scratch_pool)
{
	//All delta processing is done.
#if VERBOSE_REPLAY
	fprintf(stderr, "close_edit(%p)\n", edit_baton);
#endif
	return SVN_NO_ERROR;
}
static svn_error_t* abort_edit(void *edit_baton, apr_pool_t *scratch_pool)
{
	//The editor-driver has decided to bail out. 
#if VERBOSE_REPLAY
	fprintf(stderr, "abort_edit(%p)\n", edit_baton);
#endif
	return SVN_NO_ERROR;
}

static svn_error_t* RevStart(svn_revnum_t revnum, void* batonData, const svn_delta_editor_t** editor, void** editBatonData, apr_hash_t* revprops, apr_pool_t* pool)
{
#if VERBOSE_REPLAY
	fprintf(stderr, "Start Rev: %lu\n", revnum);
#endif

	ReplayBaton* baton = static_cast<ReplayBaton*>(batonData);

	*editBatonData = apr_palloc(pool, sizeof(EditBaton));
	EditBaton* editBaton = new(*editBatonData) EditBaton;

	editBaton->m_rev.m_revision = revnum;
	editBaton->m_subtree = baton->m_subtree;

	// Put author, date etc. into the revision structure.
	ReadRevProps(editBaton->m_rev, revprops);

	svn_delta_editor_t* myeditor = svn_delta_default_editor(pool);

	myeditor->set_target_revision = &set_target_revision;
	myeditor->open_root = &open_root;
	myeditor->delete_entry = &delete_entry;
	myeditor->add_directory = &add_directory;
	myeditor->open_directory = &open_directory;
	myeditor->change_dir_prop = &change_dir_prop;
	myeditor->close_directory = &close_directory;
	myeditor->absent_directory = &absent_directory;
	myeditor->add_file = &add_file;
	myeditor->open_file = &open_file;
	myeditor->apply_textdelta = &apply_textdelta;
	myeditor->change_file_prop = &change_file_prop;
	myeditor->close_file = &close_file;
	myeditor->absent_file = &absent_file;
	myeditor->close_edit = &close_edit;
	myeditor->abort_edit = &abort_edit;

	*editor = myeditor;

	return SVN_NO_ERROR;
}

static svn_error_t* RevEnd(svn_revnum_t revnum, void* batonData, const svn_delta_editor_t* editor, void* editBatonData, apr_hash_t* revprops, apr_pool_t* pool)
{
#if VERBOSE_REPLAY
	fprintf(stderr, "End Rev: %lu\n", revnum);
#endif
	EditBaton* editBaton = static_cast<EditBaton*>(editBatonData);
	ReplayBaton* baton = static_cast<ReplayBaton*>(batonData);
	std::vector<SVNSimple::Revision>* log = baton->m_log;

	if(editBaton->m_rev.m_files.size())
	{
		log->push_back(editBaton->m_rev);
	}

	editBaton->~EditBaton();

	return SVN_NO_ERROR;
}

void SVNSimple::Replay(std::vector<Revision>& log, svn_revnum_t from, svn_revnum_t to, bool expandDirectories)
{
	svn_error_t* err;
	apr_pool_t* pool = svn_pool_create(m_pool);

	ReplayBaton baton;
	baton.m_log = &log;
	// Replay does not prepend '/' to paths
	std::string subtree = m_subtree.substr(m_subtree[0] == '/'? 1 : 0);
	baton.m_subtree = &subtree;

	if((err = svn_ra_replay_range(
		m_session,
		from,
		to,
		0,
		FALSE,
		&RevStart,
		&RevEnd,
		&baton,
		pool
	))) {
		throw EXCEPTION(("SVN Error: %s", err->message));
	}

#if VERBOSE_REPLAY
	for(std::vector<Revision>::const_iterator it = log.begin(); it != log.end(); ++it)
	{
		fprintf(stderr, "----------- Revision %lu: %s\n%s\n", it->m_revision, it->m_user.c_str(), it->m_log.c_str());
		for(std::vector<Revision::File>::const_iterator jt = it->m_files.begin(); jt != it->m_files.end(); ++jt)
		{
			fprintf(stderr, "\t%c%c%c %s\n", jt->m_action, jt->m_type, jt->m_expand? '+' : ' ', jt->m_relPath.c_str());
		}
	}
#endif

	if(expandDirectories) {
		ExpandDirectories(log);
	}

	apr_pool_destroy(pool);
}

