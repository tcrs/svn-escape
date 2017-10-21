#ifndef FASTEXPORT_H__
#define FASTEXPORT_H__

#include "SVNSimple.h"

#include <string>
#include <vector>

class FastExport
{
public:
	FastExport(std::string const& commitRef, std::string const& parentSHA);
	~FastExport();

	void DumpRevisions(SVNSimple& connection, std::vector<SVNSimple::Revision>& revisions);

	svn_revnum_t GetLastRevisionCommitted() const { return m_lastRevisionCommitted; }

protected:
	void MakeCommit(SVNSimple::Revision const& rev);

	std::string m_commitRef;
	std::string m_parentSHA;
	svn_revnum_t m_lastRevisionCommitted;
};

#endif
