#include "FastExport.h"
#include "Exception.h"

extern "C" {
#include <svn_types.h>
}

#define LF "\x0A"

FastExport::FastExport(std::string const& commitRef, std::string const& parentSHA) :
	m_commitRef(commitRef),
	m_parentSHA(parentSHA),
	m_lastRevisionCommitted(SVN_INVALID_REVNUM)
{
}

FastExport::~FastExport() { }

void FastExport::DumpRevisions(SVNSimple& connection, std::vector<SVNSimple::Revision>& revisions)
{
	for(std::vector<SVNSimple::Revision>::const_iterator rit = revisions.begin(); rit != revisions.end(); ++rit)
	{
		SVNSimple::Revision const& rev = *rit;

		if(rev.m_files.size() == 0) {
			printf("# Skipping revision %lu; no files in commit" LF, rev.m_revision);
		} else {
			printf("# ========== Start of revision %lu" LF, rev.m_revision);
			printf("progress Getting file data for revision %lu" LF, rev.m_revision);

			unsigned long fileMark = rev.m_revision + 1;
			unsigned int numFiles = 0;
			for(std::vector<SVNSimple::Revision::File>::const_iterator fit = rev.m_files.begin(); fit != rev.m_files.end(); ++fit)
			{
				SVNSimple::Revision::File const& file = *fit;
				switch(file.m_action) {
					case 'A':
					case 'M':
					case 'R':
					case 'C': {
						if(file.m_type == 'F') {
							printf("# %c %s" LF, file.m_action, file.m_relPath.c_str());
							printf("blob" LF);
							printf("mark :%lu" LF, fileMark);
							connection.CatFile(file.m_relPath.c_str(), rev.m_revision);

							numFiles += 1;
						}
						break;
					}
					case 'D':
						numFiles += 1;
						break;
					case 'I':
						break;
					default:
						printf("# Unknown thing: %c %s" LF, file.m_action, file.m_relPath.c_str());
				}

				fileMark += 1;
			}

			if(numFiles == 0) {
				printf("# Skipping revision %lu; no files in commit" LF, rev.m_revision);
			} else {
				printf("progress Committing revision %lu" LF, rev.m_revision);
				printf("# Dumped all file data, making commit for revision %lu" LF, rev.m_revision);
				MakeCommit(rev);
				printf("# ========== End of revision %lu" LF, rev.m_revision);

				m_lastRevisionCommitted = rev.m_revision;
			}
		}
	}
}

void FastExport::MakeCommit(SVNSimple::Revision const& rev)
{
	printf("commit %s" LF, m_commitRef.c_str());
	printf("mark :%lu" LF, rev.m_revision);
	printf("committer %s %ld +0000" LF, rev.m_user.c_str(), rev.m_date);
	printf("data %lu" LF, rev.m_log.size());
	if(rev.m_log.size()) {
		fwrite(rev.m_log.c_str(), 1, rev.m_log.size(), stdout);
	}
	if(m_lastRevisionCommitted == SVN_INVALID_REVNUM && m_parentSHA.size()) {
		printf("from %s" LF, m_parentSHA.c_str());
	}

	unsigned long fileMark = rev.m_revision + 1;
	for(std::vector<SVNSimple::Revision::File>::const_iterator fit = rev.m_files.begin(); fit != rev.m_files.end(); ++fit)
	{
		SVNSimple::Revision::File const& file = *fit;
		if(file.m_type == 'F' || file.m_action == 'D') {
			switch(file.m_action)
			{
				case 'M':
				case 'A':
				case 'C':
					printf("M 644 :%lu %s" LF, fileMark, file.m_relPath.c_str());
					break;
				case 'D':
					printf("D %s" LF, file.m_relPath.c_str());
					break;
				case 'R':
					printf("D %s" LF, file.m_relPath.c_str());
					printf("M 100644 :%lu %s" LF, fileMark, file.m_relPath.c_str());
					break;
				default:
					break;
			}
		}

		fileMark += 1;
	}

	printf(LF);
}
