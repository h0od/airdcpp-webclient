/*
 * Copyright (C) 2001-2011 Jacek Sieka, arnetheduck on gmail point com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <string>
#include "stdinc.h"
#include "ShareManager.h"

#include "ResourceManager.h"

#include "CryptoManager.h"
#include "ClientManager.h"
#include "LogManager.h"
#include "HashManager.h"
#include "QueueManager.h"

#include "SimpleXML.h"
#include "StringTokenizer.h"
#include "File.h"
#include "FilteredFile.h"
#include "BZUtils.h"
#include "Transfer.h"
#include "UserConnection.h"
#include "Download.h"
#include "HashBloom.h"
#include "SearchResult.h"
#include "Wildcards.h"
#include "AirUtil.h"

#include <boost/range/algorithm/remove_if.hpp>
#include <boost/range/algorithm/for_each.hpp>
#include <boost/algorithm/cxx11/copy_if.hpp>
#include <boost/range/algorithm/search.hpp>
#include <boost/range/adaptor/map.hpp>
#include <boost/range/algorithm/find_if.hpp>

#ifdef _WIN32
# include <ShlObj.h>
#else
# include <dirent.h>
# include <sys/stat.h>
# include <unistd.h>
# include <fnmatch.h>
#endif

#include <limits>

namespace dcpp {

using std::string;
using boost::adaptors::map_values;

#define SHARE_CACHE_VERSION "1"

atomic_flag ShareManager::refreshing = ATOMIC_FLAG_INIT;

ShareManager::ShareManager() : lastFullUpdate(GET_TICK()), lastIncomingUpdate(GET_TICK()), bloom(1<<20), sharedSize(0), ShareCacheDirty(false),
	xml_saving(false), lastSave(GET_TICK()), aShutdown(false), allSearches(0), stoppedSearches(0), refreshRunning(false)
{ 
	SettingsManager::getInstance()->addListener(this);
	TimerManager::getInstance()->addListener(this);
	QueueManager::getInstance()->addListener(this);

	RAR_regexp.Init("[Rr0-9][Aa0-9][Rr0-9]");
	subDirRegPlain.assign("(((DVD)|(CD)|(DIS(K|C))).?([0-9](0-9)?))|(Sample)|(Proof)|(Cover(s)?)|(.{0,5}Sub(s|pack)?)", boost::regex::icase);

#ifdef _WIN32
	// don't share Windows directory
	TCHAR path[MAX_PATH];
	::SHGetFolderPath(NULL, CSIDL_WINDOWS, NULL, SHGFP_TYPE_CURRENT, path);
	winDir = Text::toLower(Text::fromT(path)) + PATH_SEPARATOR;
#endif
}

ShareManager::~ShareManager() {

	SettingsManager::getInstance()->removeListener(this);
	TimerManager::getInstance()->removeListener(this);
	QueueManager::getInstance()->removeListener(this);

	join();
	w.join();
}

void ShareManager::startup() {
	AirUtil::updateCachedSettings();
	if (!getShareProfile(SP_DEFAULT)) {
		ShareProfilePtr sp = ShareProfilePtr(new ShareProfile(STRING(DEFAULT), SP_DEFAULT));
		shareProfiles.push_back(sp);
	}

	ShareProfilePtr hidden = ShareProfilePtr(new ShareProfile("Hidden", SP_HIDDEN));
	shareProfiles.push_back(hidden);

	if(!loadCache()) {
		refresh(false);
	}
	rebuildExcludeTypes();
	setSkipList();
}

void ShareManager::shutdown() {
	//abort buildtree and refresh, we are shutting down.
	aShutdown = true;

	if(ShareCacheDirty || !Util::fileExists(Util::getPath(Util::PATH_USER_CONFIG) + "Shares.xml"))
		saveXmlList();

	try {
		StringList lists = File::findFiles(Util::getPath(Util::PATH_USER_CONFIG), "files?*.xml.bz2");
		//clear refs so we can delete filelists.
		RLock l(cs);
		for(auto f = shareProfiles.begin(); f != shareProfiles.end(); ++f) {
			if((*f)->getProfileList() && (*f)->getProfileList()->bzXmlRef.get()) 
				(*f)->getProfileList()->bzXmlRef.reset(); 
		}

		for(auto i = lists.begin(); i != lists.end(); ++i) {
			File::deleteFile(*i); // cannot delete the current filelist due to the bzxmlref.
		}
		lists.clear();
		
		//leave the latest filelist undeleted, and rename it to files.xml.bz2
		/*FileList* fl =  fileLists.find(FileListALL)->second;
		
		if(fl->bzXmlRef.get()) 
			fl->bzXmlRef.reset(); 

			if(!Util::fileExists(Util::getPath(Util::PATH_USER_CONFIG) + "files.xml.bz2"))				
				File::renameFile(fl->getBZXmlFile(), ( Util::getPath(Util::PATH_USER_CONFIG) + "files.xml.bz2") ); */
				
	} catch(...) {
		//ignore, we just failed to delete
	}
		
}

void ShareManager::setDirty(bool force /*false*/) {
	for(auto i = shareProfiles.begin(); i != shareProfiles.end(); ++i) {
		if ((*i)->getProfileList()) {
			(*i)->getProfileList()->setXmlDirty(true);
			if(force)
				(*i)->getProfileList()->setForceXmlRefresh(true);
		}
	}
	ShareCacheDirty = true; 
}

void ShareManager::setDirty(ProfileToken aProfile) {
	RLock l(cs);
	auto i = find(shareProfiles.begin(), shareProfiles.end(), aProfile);
	if(i != shareProfiles.end())
		(*i)->getProfileList()->setForceXmlRefresh(true);
}

ShareManager::Directory::Directory(const string& aRealName, const ShareManager::Directory::Ptr& aParent, uint32_t aLastWrite, ProfileDirectory::Ptr aProfileDir) :
	size(0),
	realName(aRealName),
	parent(aParent.get()),
	fileTypes(1 << SearchManager::TYPE_DIRECTORY),
	profileDir(aProfileDir),
	lastWrite(aLastWrite)
{
}
int64_t ShareManager::Directory::getSize(ProfileToken aProfile) const noexcept {
	int64_t tmp = size;
	for(auto i = directories.begin(); i != directories.end(); ++i) {
		if (i->second->isLevelExcluded(aProfile))
			continue;
		tmp += i->second->getSize(aProfile);
	}
	return tmp;
}

int64_t ShareManager::Directory::getTotalSize() const noexcept {
	int64_t tmp = size;
	for(auto i = directories.begin(); i != directories.end(); ++i) {
		tmp += i->second->getTotalSize();
	}
	return tmp;
}

string ShareManager::Directory::getADCPath(ProfileToken aProfile) const noexcept {
	if(profileDir && profileDir->hasProfile(aProfile))
		return '/' + profileDir->getName(aProfile) + '/';
	return parent->getADCPath(aProfile) + realName + '/';
}

string ShareManager::Directory::getVirtualName(ProfileToken aProfile) const noexcept {
	if(profileDir && profileDir->hasProfile(aProfile))
		return profileDir->getName(aProfile);
	return realName;
}

string ShareManager::Directory::getFullName(ProfileToken aProfile) const noexcept {
	if(profileDir && profileDir->hasProfile(aProfile))
		return profileDir->getName(aProfile) + '\\';
	dcassert(parent);
	return parent->getFullName(aProfile) + realName + '\\';
}

void ShareManager::Directory::addType(uint32_t type) noexcept {
	if(!hasType(type)) {
		fileTypes |= (1 << type);
		if(parent)
			parent->addType(type);
	}
}

string ShareManager::getRealPath(const TTHValue& root) {
	RLock l(cs);
	auto i = tthIndex.find(const_cast<TTHValue*>(&root)); 
	if(i != tthIndex.end()) {
		return i->second->getRealPath();
	}
	return Util::emptyString;
}


bool ShareManager::isTTHShared(const TTHValue& tth) {
	RLock l(cs);
	return tthIndex.find(const_cast<TTHValue*>(&tth)) != tthIndex.end();
}

string ShareManager::Directory::getRealPath(const string& path, bool checkExistance /*true*/) const {
	if(getParent()) {
		return getParent()->getRealPath(realName + PATH_SEPARATOR_STR + path, checkExistance);
	}

	string rootDir = getProfileDir()->getPath() + path;

	if(!checkExistance) //no extra checks for finding the file while loading share cache.
		return rootDir;

	/*check for the existance here if we have moved the file/folder and only refreshed the new location.
	should we even look, what's moved is moved, user should refresh both locations.*/
	if(Util::fileExists(rootDir))
		return rootDir;
	else
		return ShareManager::getInstance()->findRealRoot(realName, path); // all display names should be compared really..
}

string ShareManager::findRealRoot(const string& virtualRoot, const string& virtualPath) const {
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		auto& profiles = i->second->getProfileDir()->getShareProfiles();
		for(auto k = profiles.begin(); k != profiles.end(); ++k) {
			if(stricmp(k->second, virtualRoot) == 0) {
				string name = k->second + virtualPath;
				dcdebug("Matching %s\n", name.c_str());
				if(FileFindIter(name) != FileFindIter()) {
					return name;
				}
			}
		}
	}
	
	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

bool ShareManager::Directory::isRootLevel(ProfileToken aProfile) {
	return profileDir && profileDir->hasProfile(aProfile) ? true : false;
}

bool ShareManager::Directory::hasProfile(const ProfileTokenSet& aProfiles) {
	if (profileDir && profileDir->hasProfile(aProfiles))
		return true;
	if (parent)
		return parent->hasProfile(aProfiles);
	return false;
}

bool ShareManager::ProfileDirectory::hasProfile(const ProfileTokenSet& aProfiles) {
	for(auto i = aProfiles.begin(); i != aProfiles.end(); ++i) {
		if (shareProfiles.find(*i) != shareProfiles.end())
			return true;
	}
	return false;
}

bool ShareManager::Directory::hasProfile(ProfileToken aProfile) {
	if(profileDir) {
		if (isLevelExcluded(aProfile))
			return false;
		if (profileDir->hasProfile(aProfile))
			return true;
	} 
	
	if (parent) {
		return parent->hasProfile(aProfile);
	}
	return false;
}

bool ShareManager::ProfileDirectory::hasProfile(ProfileToken aProfile) {
	return shareProfiles.find(aProfile) != shareProfiles.end();
}

bool ShareManager::Directory::isLevelExcluded(ProfileToken aProfile) {
	if (profileDir && profileDir->isExcluded(aProfile))
		return true;
	return false;
}

bool ShareManager::ProfileDirectory::isExcluded(ProfileToken aProfile) {
	return !excludedProfiles.empty() && excludedProfiles.find(aProfile) != excludedProfiles.end();
}

ShareManager::ProfileDirectory::ProfileDirectory(const string& aRootPath, const string& aVname, ProfileToken aProfile) : path(aRootPath) { 
	shareProfiles[aProfile] = aVname;
	setFlag(FLAG_ROOT);
}

ShareManager::ProfileDirectory::ProfileDirectory(const string& aRootPath, ProfileToken aProfile) : path(aRootPath) { 
	excludedProfiles.insert(aProfile);
	setFlag(FLAG_EXCLUDE_PROFILE);
}

void ShareManager::ProfileDirectory::addRootProfile(const string& aName, ProfileToken aProfile) { 
	shareProfiles[aProfile] = aName;
	setFlag(FLAG_ROOT);
}

void ShareManager::ProfileDirectory::addExclude(ProfileToken aProfile) {
	setFlag(FLAG_EXCLUDE_PROFILE);
	excludedProfiles.insert(aProfile);
}

bool ShareManager::ProfileDirectory::removeRootProfile(ProfileToken aProfile) {
	shareProfiles.erase(aProfile);
	return shareProfiles.empty();
}

string ShareManager::ProfileDirectory::getName(ProfileToken aProfile) {
	auto p = shareProfiles.find(aProfile);
	return p == shareProfiles.end() ? Util::emptyString : p->second; 
}

string ShareManager::toVirtual(const TTHValue& tth, ProfileToken aProfile) const  {
	
	RLock l(cs);

	FileList* fl = getFileList(aProfile);
	if(tth == fl->getBzXmlRoot()) {
		return Transfer::USER_LIST_NAME_BZ;
	} else if(tth == fl->getXmlRoot()) {
		return Transfer::USER_LIST_NAME;
	}

	auto i = tthIndex.find(const_cast<TTHValue*>(&tth)); 
	if(i != tthIndex.end()) 
		return i->second->getADCPath(aProfile);

	//nothing found throw;
	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

FileList* ShareManager::getFileList(ProfileToken aProfile) const{
	auto i = find(shareProfiles.begin(), shareProfiles.end(), aProfile);
	if(i != shareProfiles.end()) {
		dcassert((*i)->getProfileList());
		return (*i)->getProfileList();
	}

	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
	
	//return shareProfiles[SP_DEFAULT]->second->getProfileList();
}

pair<string, int64_t> ShareManager::toRealWithSize(const string& virtualFile, ProfileToken aProfile) {
	if(virtualFile == "MyList.DcLst") 
		throw ShareException("NMDC-style lists no longer supported, please upgrade your client");

	if(virtualFile == Transfer::USER_LIST_NAME_BZ || virtualFile == Transfer::USER_LIST_NAME) {
		FileList* fl = generateXmlList(aProfile);
		return make_pair(fl->getFileName(), 0);
	}

	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

pair<string, int64_t> ShareManager::toRealWithSize(const string& virtualFile, const ProfileTokenSet& aProfiles, const HintedUser& aUser) {
	if(virtualFile.compare(0, 4, "TTH/") == 0) {
		TTHValue tth(virtualFile.substr(4));

		/* Only check temp files if the share has been hidden */
		if(any_of(aProfiles.begin(), aProfiles.end(), [](ProfileToken s) { return s != SP_HIDDEN; })) {
			RLock l(cs);
			auto flst = tthIndex.equal_range(const_cast<TTHValue*>(&tth));
			for(auto f = flst.first; f != flst.second; ++f) {
				if(f->second->getParent()->hasProfile(aProfiles)) {
					return make_pair(f->second->getRealPath(), f->second->getSize());
				}
			}
		}

		Lock l(tScs);
		auto files = tempShares.equal_range(tth);
		for(auto i = files.first; i != files.second; ++i) {
			if(i->second.key.empty() || (i->second.key == aUser.user->getCID().toBase32())) // if no key is set, it means its a hub share.
				return make_pair(i->second.path, i->second.size);
		}
	}

	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

TTHValue ShareManager::getListTTH(const string& virtualFile, ProfileToken aProfile) const {
	RLock l(cs);
	if(virtualFile == Transfer::USER_LIST_NAME_BZ) {
		return getFileList(aProfile)->getBzXmlRoot();
	} else if(virtualFile == Transfer::USER_LIST_NAME) {
		return getFileList(aProfile)->getXmlRoot();
	}

	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

MemoryInputStream* ShareManager::getTree(const string& virtualFile, ProfileToken aProfile) const {
	TigerTree tree;
	if(virtualFile.compare(0, 4, "TTH/") == 0) {
		if(!HashManager::getInstance()->getTree(TTHValue(virtualFile.substr(4)), tree))
			return 0;
	} else {
		try {
			TTHValue tth = getListTTH(virtualFile, aProfile);
			HashManager::getInstance()->getTree(tth, tree);
		} catch(const Exception&) {
			return 0;
		}
	}

	ByteVector buf = tree.getLeafData();
	return new MemoryInputStream(&buf[0], buf.size());
}

AdcCommand ShareManager::getFileInfo(const string& aFile, ProfileToken aProfile) {
	if(aFile == Transfer::USER_LIST_NAME) {
		FileList* fl = generateXmlList(aProfile);
		AdcCommand cmd(AdcCommand::CMD_RES);
		cmd.addParam("FN", aFile);
		cmd.addParam("SI", Util::toString(fl->getXmlListLen()));
		cmd.addParam("TR", fl->getXmlRoot().toBase32());
		return cmd;
	} else if(aFile == Transfer::USER_LIST_NAME_BZ) {
		FileList* fl = generateXmlList(aProfile);
		AdcCommand cmd(AdcCommand::CMD_RES);
		cmd.addParam("FN", aFile);
		cmd.addParam("SI", Util::toString(fl->getBzXmlListLen()));
		cmd.addParam("TR", fl->getBzXmlRoot().toBase32());
		return cmd;
	}

	if(aFile.compare(0, 4, "TTH/") != 0)
		throw ShareException(UserConnection::FILE_NOT_AVAILABLE);

	TTHValue val(aFile.substr(4));
	
	RLock l(cs);
	auto i = tthIndex.find(const_cast<TTHValue*>(&val)); 
	if(i != tthIndex.end()) {
		const Directory::File& f = *i->second;
		AdcCommand cmd(AdcCommand::CMD_RES);
		cmd.addParam("FN", f.getADCPath(aProfile));
		cmd.addParam("SI", Util::toString(f.getSize()));
		cmd.addParam("TR", f.getTTH().toBase32());
		return cmd;
	}

	//not found throw
	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
}

ShareManager::TempShareInfo ShareManager::findTempShare(const string& aKey, const string& virtualFile) {
	if(virtualFile.compare(0, 4, "TTH/") == 0) {
		Lock l(tScs);
		TTHValue tth(virtualFile.substr(4));
		auto Files = tempShares.equal_range(tth);
		for(auto i = Files.first; i != Files.second; ++i) {
			if(i->second.key.empty() || (i->second.key == aKey)) // if no key is set, it means its a hub share.
				return i->second;
		}
	}	
	throw ShareException(UserConnection::FILE_NOT_AVAILABLE);		
}

bool ShareManager::addTempShare(const string& aKey, TTHValue& tth, const string& filePath, int64_t aSize, bool adcHub) {
	//first check if already exists in Share.
	if(isFileShared(tth, Util::getFileName(filePath))) {
		return true;
	} else if(adcHub) {
		Lock l(tScs);
		auto Files = tempShares.equal_range(tth);
		for(auto i = Files.first; i != Files.second; ++i) {
			if(i->second.key == aKey)
				return true;
		}
		//didnt exist.. fine, add it.
		tempShares.insert(make_pair(tth, TempShareInfo(aKey, filePath, aSize)));
		return true;
	}
	return false;
}
void ShareManager::removeTempShare(const string& aKey, TTHValue& tth) {
	Lock l(tScs);
	auto Files = tempShares.equal_range(tth);
	for(auto i = Files.first; i != Files.second; ++i) {
		if(i->second.key == aKey) {
			tempShares.erase(i);
			break;
		}
	}
}

void ShareManager::findVirtuals(const string& virtualPath, ProfileToken aProfile, DirectoryList& dirs) const {

	DirectoryList virtuals; //since we are mapping by realpath, we can have more than 1 same virtualnames
	if(virtualPath.empty() || virtualPath[0] != '/') {
		throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
	}

	string::size_type start = virtualPath.find('/', 1);
	if(start == string::npos || start == 1) {
		throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
	}

	getByVirtual( virtualPath.substr(1, start-1), aProfile, virtuals);
	if(virtuals.empty()) {
		throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
	}

	Directory::Ptr d;
	for(auto k = virtuals.begin(); k != virtuals.end(); k++) {
		string::size_type i = start; // always start from the begin.
		string::size_type j = i + 1;
		d = *k;

		if(virtualPath.find('/', j) == string::npos) {	  // we only have root virtualpaths.
			dirs.push_back(d);
		} else {
			while((i = virtualPath.find('/', j)) != string::npos) {
				if(d) {
					auto mi = d->directories.find(virtualPath.substr(j, i - j));
					j = i + 1;
					if(mi != d->directories.end() && !mi->second->isLevelExcluded(aProfile)) {   //if we found something, look for more.
						d = mi->second;
					} else {
						d = nullptr;   //make the pointer null so we can check if found something or not.
						break;
					}
				}
			}

			if(d) 
				dirs.push_back(d);
		}
	}

	if(dirs.empty()) {
		//if we are here it means we didnt find anything, throw.
		throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
	}
}

void ShareManager::getRealPaths(const string& path, StringList& ret, ProfileToken aProfile) {
	if(path.empty())
		throw ShareException("empty virtual path");

	DirectoryList dirs;

	RLock l (cs);
	findVirtuals(path, aProfile, dirs);

	if(*(path.end() - 1) == '/') {
		for(auto i = dirs.begin(); i != dirs.end(); ++i) {
			ret.push_back((*i)->getRealPath(true));
		}
	} else { //its a file
		for(auto v = dirs.begin(); v != dirs.end(); ++v) {
			auto it = find_if((*v)->files.begin(), (*v)->files.end(), Directory::File::StringComp(Util::getFileName(path)));
			if(it != (*v)->files.end()) {
				ret.push_back(it->getRealPath());
				return;
			}
		}
	}
}
string ShareManager::validateVirtual(const string& aVirt) const noexcept {
	string tmp = aVirt;
	string::size_type idx = 0;

	while( (idx = tmp.find_first_of("\\/"), idx) != string::npos) {
		tmp[idx] = '_';
	}
	return tmp;
}

void ShareManager::loadProfile(SimpleXML& aXml, const string& aName, ProfileToken aToken) {
	ShareProfilePtr sp = ShareProfilePtr(new ShareProfile(aName, aToken));
	shareProfiles.push_back(sp);

	aXml.stepIn();
	while(aXml.findChild("Directory")) {
		string realPath = aXml.getChildData();
		if(realPath.empty()) {
			continue;
		}
		// make sure realPath ends with PATH_SEPARATOR
		if(realPath[realPath.size() - 1] != PATH_SEPARATOR) {
			realPath += PATH_SEPARATOR;
		}

		const string& virtualName = aXml.getChildAttrib("Virtual");
		string vName = validateVirtual(virtualName.empty() ? Util::getLastDir(realPath) : virtualName);

		ProfileDirectory::Ptr pd = nullptr;
		auto p = profileDirs.find(realPath);
		if (p != profileDirs.end()) {
			pd = p->second;
			pd->addRootProfile(virtualName, aToken);
		} else {
			pd = ProfileDirectory::Ptr(new ProfileDirectory(realPath, virtualName, aToken));
			profileDirs[realPath] = pd;
		}

		auto j = shares.find(realPath);
		if (j == shares.end()) {
			auto dir = Directory::create(virtualName, nullptr, 0, pd);
			shares[realPath] = dir;
		}

		if (aXml.getBoolChildAttrib("Incoming"))
			pd->setFlag(ProfileDirectory::FLAG_INCOMING);
	}

	aXml.resetCurrentChild();
	if(aXml.findChild("NoShare")) {
		aXml.stepIn();
		while(aXml.findChild("Directory")) {
			auto path = aXml.getChildData();

			//ProfileDirectory::Ptr pd = nullptr;
			auto p = profileDirs.find(path);
			if (p != profileDirs.end()) {
				auto pd = p->second;
				pd->addExclude(aToken);
			} else {
				auto pd = ProfileDirectory::Ptr(new ProfileDirectory(path, aToken));
				profileDirs[path] = pd;
			}
		}
		aXml.stepOut();
	}
	aXml.stepOut();
}

void ShareManager::load(SimpleXML& aXml) {
	//WLock l(cs);
	aXml.resetCurrentChild();

	if(aXml.findChild("Share")) {
		string name = aXml.getChildAttrib("Name");
		loadProfile(aXml, !name.empty() ? name : STRING(DEFAULT), SP_DEFAULT);
	}

	aXml.resetCurrentChild();
	while(aXml.findChild("ShareProfile")) {
		auto token = aXml.getIntChildAttrib("Token");
		string name = aXml.getChildAttrib("Name");
		if (token > 10 && !name.empty()) //reserve a few numbers for predefined profiles
			loadProfile(aXml, name, token);
	}
}

ShareProfilePtr ShareManager::getShareProfile(ProfileToken aProfile, bool allowFallback /*false*/) {
	RLock l (cs);
	auto p = find(shareProfiles.begin(), shareProfiles.end(), aProfile);
	if (p != shareProfiles.end()) {
		return *p;
	} else if (allowFallback) {
		dcassert(aProfile != SP_DEFAULT);
		return *shareProfiles.begin();
	}
	return nullptr;
}

static const string SDIRECTORY = "Directory";
static const string SFILE = "File";
static const string SNAME = "Name";
static const string SSIZE = "Size";
static const string STTH = "TTH";
static const string PATH = "Path";
static const string DATE = "Date";

struct ShareLoader : public SimpleXMLReader::CallBack {
	ShareLoader(ShareManager::ProfileDirMap& aDirs) : profileDirs(aDirs), cur(nullptr)/*, depth(0), blockNode(false)*/ { }
	void startTag(const string& name, StringPairList& attribs, bool simple) {

		if(name == SDIRECTORY) {
			/*if (!blockNode || depth == 0) {
				blockNode = false;*/
				const string& name = getAttrib(attribs, SNAME, 0);
				curDirPath = getAttrib(attribs, PATH, 1);
				const string& date = getAttrib(attribs, DATE, 2);

				if(curDirPath[curDirPath.length() - 1] != PATH_SEPARATOR)
					curDirPath += PATH_SEPARATOR;

				if(!name.empty()) {
					cur = ShareManager::Directory::create(name, cur, Util::toUInt32(date));
					auto i = profileDirs.find(curDirPath);
					if(i != profileDirs.end()) {
						cur->setProfileDir(i->second);
						if (i->second->hasRoots())
							ShareManager::getInstance()->addShares(curDirPath, cur);
					} /*else if (depth == 0) {
						//something wrong...
						cur = nullptr;
						blockNode = true;
						depth++;
						return;
					}*/

					dirs.insert(make_pair(name, cur));
					lastFileIter = cur->files.begin();
				}
			//}

			if(simple) {
				if(cur) {
					cur = cur->getParent();
					if(cur)
						lastFileIter = cur->files.begin();
				}
			} /*else {
				depth++;
			}*/
		} else if(cur && name == SFILE) {
			const string& fname = getAttrib(attribs, SNAME, 0);
			const string& size = getAttrib(attribs, SSIZE, 1);   
			if(fname.empty() || size.empty() ) {
				dcdebug("Invalid file found: %s\n", fname.c_str());
				return;
			}
			/*dont save TTHs, check them from hashmanager, just need path and size.
			this will keep us sync to hashindex */
			try {
				lastFileIter = cur->files.insert(lastFileIter, ShareManager::Directory::File(fname, Util::toInt64(size), cur, HashManager::getInstance()->getTTH(curDirPath + fname, Util::toInt64(size))));
			}catch(Exception& e) { 
				dcdebug("Error loading filelist %s \n", e.getError().c_str());
			}
		}
	}
	void endTag(const string& name, const string&) {
		if(name == SDIRECTORY) {
			//depth--;
			if(cur) {
				curDirPath = Util::getParentDir(curDirPath);
				cur = cur->getParent();
				if(cur)
					lastFileIter = cur->files.begin();
			}
		}
	}

	ShareManager::DirMultiMap dirs;
private:
	ShareManager::ProfileDirMap& profileDirs;

	ShareManager::Directory::File::Set::iterator lastFileIter;
	ShareManager::Directory::Ptr cur;

	//bool blockNode;
	//size_t depth;
	string curDirPath;
};

bool ShareManager::loadCache() {
	try {
		ShareLoader loader(profileDirs);
		//look for shares.xml
		dcpp::File ff(Util::getPath(Util::PATH_USER_CONFIG) + "Shares.xml", dcpp::File::READ, dcpp::File::OPEN, false);
		SimpleXMLReader(&loader).parse(ff);
		dirNameMap = loader.dirs;

		rebuildIndices();
	}catch(SimpleXMLException& e) {
		LogManager::getInstance()->message("Error Loading shares.xml: "+ e.getError(), LogManager::LOG_ERROR);
		return false;
	} catch(...) {
		return false;
	}

	/*try { //not vital to our cache loading.
		auto fl = getFileList(SP_DEFAULT);
		fl->setBZXmlFile( Util::getPath(Util::PATH_USER_CONFIG) + "files.xml.bz2");
		if(!Util::fileExists(fl->getBZXmlFile())) {  //only generate if we dont find old filelist
			generateXmlList(SP_DEFAULT, true);
		} else {
			fl->bzXmlRef = unique_ptr<File>(new File(fl->getBZXmlFile(), File::READ, File::OPEN));
		}
	} catch(...) { }*/

	return true;
}

void ShareManager::save(SimpleXML& aXml) {
	RLock l(cs);
	for(auto i = shareProfiles.begin(); i != shareProfiles.end(); ++i) {
		if ((*i)->getToken() == SP_HIDDEN) {
			continue;
		}

		aXml.addTag((*i)->getToken() == SP_DEFAULT ? "Share" : "ShareProfile");
		aXml.addChildAttrib("Token", (*i)->getToken());
		aXml.addChildAttrib("Name", (*i)->getPlainName());
		aXml.stepIn();

		for(auto p = shares.begin(); p != shares.end(); ++p) {
			if (!p->second->getProfileDir()->hasProfile((*i)->getToken()))
				continue;
			aXml.addTag("Directory", p->first);
			aXml.addChildAttrib("Virtual", p->second->getProfileDir()->getName((*i)->getToken()));
			//if (p->second->getRoot()->hasFlag(ProfileDirectory::FLAG_INCOMING))
			aXml.addChildAttrib("Incoming", p->second->getProfileDir()->isSet(ProfileDirectory::FLAG_INCOMING));
		}

		aXml.addTag("NoShare");
		aXml.stepIn();
		for(auto j = profileDirs.begin(); j != profileDirs.end(); ++j) {
			if (j->second->isExcluded((*i)->getToken()))
				aXml.addTag("Directory", j->second->getPath());
		}
		aXml.stepOut();
		aXml.stepOut();
	}
}

void ShareManager::validatePath(const string& realPath, const string& virtualName) {
	if(realPath.empty() || virtualName.empty()) {
		throw ShareException(STRING(NO_DIRECTORY_SPECIFIED));
	}

	if (!checkHidden(realPath)) {
		throw ShareException(STRING(DIRECTORY_IS_HIDDEN));
	}

	if(stricmp(SETTING(TEMP_DOWNLOAD_DIRECTORY), realPath) == 0) {
		throw ShareException(STRING(DONT_SHARE_TEMP_DIRECTORY));
	}

#ifdef _WIN32
	//need to throw here, so throw the error and dont use airutil
	TCHAR path[MAX_PATH];
	::SHGetFolderPath(NULL, CSIDL_WINDOWS, NULL, SHGFP_TYPE_CURRENT, path);
	string windows = Text::fromT((tstring)path) + PATH_SEPARATOR;
	// don't share Windows directory
	if(strnicmp(realPath, windows, windows.length()) == 0) {
		char buf[MAX_PATH];
		snprintf(buf, sizeof(buf), CSTRING(CHECK_FORBIDDEN), realPath.c_str());
		throw ShareException(buf);
	}
#endif
}

void ShareManager::getByVirtual(const string& virtualName, ProfileToken aProfile, DirectoryList& dirs) const throw() {
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		if((aProfile < 0 || i->second->getProfileDir()->hasProfile(aProfile)) && stricmp(i->second->getProfileDir()->getName(aProfile), virtualName) == 0) {
			dirs.push_back(i->second);
		}
	}
}

int64_t ShareManager::getShareSize(const string& realPath, ProfileToken aProfile) const noexcept {
	RLock l(cs);
	auto j = shares.find(realPath);
	if(j != shares.end()) {
		return j->second->getSize(aProfile);
	}
	return -1;

}

void ShareManager::Directory::getProfileInfo(ProfileToken aProfile, int64_t& totalSize, size_t& filesCount) const {
	totalSize += size;
	filesCount += files.size();

	for(auto i = directories.begin(); i != directories.end(); ++i) {
		if (i->second->isLevelExcluded(aProfile))
			continue;
		i->second->getProfileInfo(aProfile, totalSize, filesCount);
	}
}

void ShareManager::getProfileInfo(ProfileToken aProfile, int64_t& size, size_t& files) const {
	RLock l(cs);
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		if(i->second->getProfileDir()->hasProfile(aProfile)) {
			i->second->getProfileInfo(aProfile, size, files);
		}
	}
}

int64_t ShareManager::getTotalShareSize(ProfileToken aProfile) const noexcept {
	int64_t ret = 0;

	RLock l(cs);
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		if(i->second->getProfileDir()->hasProfile(aProfile)) {
			ret += i->second->getSize(aProfile);
		}
	}
	return ret;
}

bool ShareManager::isDirShared(const string& aDir) const {
	RLock l (cs);
	return getDirByName(aDir) ? true : false;
}

uint8_t ShareManager::isDirShared(const string& aDir, int64_t aSize) const {
	RLock l (cs);
	auto dir = getDirByName(aDir);
	if (!dir)
		return 0;
	return dir->getTotalSize() == aSize ? 2 : 1;
}

tstring ShareManager::getDirPath(const string& aDir) {
	RLock l (cs);
	auto dir = getDirByName(aDir);
	if (!dir)
		return Util::emptyStringT;

	return Text::toT(dir->getRealPath(true));
}

/* This isn't optimized for matching subdirs but there shouldn't be need to match many of those 
   at once (especially not in filelists, but there might be some when searching though) */
ShareManager::Directory::Ptr ShareManager::getDirByName(const string& aDir) const {
	if (aDir.size() < 3)
		return nullptr;

	//get the last directory, we might need the position later with subdirs
	string dir = aDir;
	if (dir[dir.length()-1] == PATH_SEPARATOR)
		dir.erase(aDir.size()-1, aDir.size());
	auto pos = dir.rfind(PATH_SEPARATOR);
	if (pos != string::npos)
		dir = dir.substr(pos+1);

	auto directories = dirNameMap.equal_range(dir);
	if (directories.first == directories.second)
		return nullptr;

	if (boost::regex_match(dir, subDirRegPlain) && pos != string::npos) {
		string::size_type i, j;
		dir = PATH_SEPARATOR + aDir;

		for(auto s = directories.first; s != directories.second; ++s) {
			//start matching from the parent dir, as we know the last one already
			i = pos;
			Directory::Ptr cur = s->second->getParent();

			for(;;) {
				if (!cur)
					break;

				j = dir.find_last_of(PATH_SEPARATOR, i);
				if(j == string::npos)
					break;

				auto remoteDir = dir.substr(j+1, i-j);
				if(stricmp(cur->getRealName(), remoteDir) == 0) {
					if (!boost::regex_match(remoteDir, subDirRegPlain)) { //another subdir? don't break in that case
						return s->second;
					}
				} else {
					//this is something different... continue to next match
					break;
				}
				cur = cur->getParent();
				i = j - 1;
			}
		}
	} else {
		return directories.first->second;
	}

	return nullptr;
}

bool ShareManager::isFileShared(const TTHValue aTTH, const string& fileName) const {
	RLock l (cs);

	for(auto m = shares.begin(); m != shares.end(); ++m) {
		auto files = tthIndex.equal_range(const_cast<TTHValue*>(&aTTH));
		for(auto i = files.first; i != files.second; ++i) {
			if(stricmp(fileName.c_str(), i->second->getName().c_str()) == 0) {
				return true;
			}
		}
	}
	return false;
}

void ShareManager::removeDir(ShareManager::Directory::Ptr aDir) {
	boost::for_each(aDir->directories | map_values, [this](Directory::Ptr d) { removeDir(d); });

	//speed this up a bit
	auto directories = dirNameMap.equal_range(aDir->getRealName());
	string realPath = aDir->getRealPath(false);

	auto p = find_if(directories.first, directories.second, [realPath](pair<string, Directory::Ptr> sdp) { return sdp.second->getRealPath(false) == realPath; });
	dcassert(p != dirNameMap.end());
	if (p != dirNameMap.end())
		dirNameMap.erase(p);
}

void ShareManager::buildTree(const string& aPath, const Directory::Ptr& aDir, bool checkQueued, const ProfileDirMap& aSubRoots, DirMultiMap& aDirs, DirMap& newShares) {
	auto lastFileIter = aDir->files.begin();
	FileFindIter end;

#ifdef _WIN32
	for(FileFindIter i(aPath + "*"); i != end && !aShutdown; ++i) {
#else
	//the fileiter just searches directorys for now, not sure if more 
	//will be needed later
	//for(FileFindIter i(aName + "*"); i != end; ++i) {
	for(FileFindIter i(aName); i != end; ++i) {
#endif
		string name = i->getFileName();
		if(name.empty()) {
			LogManager::getInstance()->message("Invalid file name found while hashing folder "+ aPath + ".", LogManager::LOG_WARNING);
			return;
		}

		if(!BOOLSETTING(SHARE_HIDDEN) && i->isHidden())
			continue;

		if(i->isDirectory()) {
			string curPath = aPath + name + PATH_SEPARATOR;

			if (!checkSharedName(curPath, true)) {
				continue;
			}

			{
				RLock l (dirNames);
				//check queue so we dont add incomplete stuff to share.
				if(checkQueued && std::binary_search(bundleDirs.begin(), bundleDirs.end(), Text::toLower(curPath))) {
					continue;
				}
			}

			ProfileDirectory::Ptr profileDir = nullptr;
			if (!aSubRoots.empty()) {
				auto p = aSubRoots.find(curPath);
				if (p != aSubRoots.end()) {
					if (p->second->isSet(ProfileDirectory::FLAG_ROOT) || p->second->isSet(ProfileDirectory::FLAG_EXCLUDE_PROFILE))
						profileDir = p->second;
					if (p->second->isSet(ProfileDirectory::FLAG_EXCLUDE_TOTAL))
						continue;
				}
			}

			auto dir = Directory::create(name, aDir, i->getLastWriteTime(), profileDir);
			aDirs.insert(make_pair(name, dir));
			if (profileDir && profileDir->isSet(ProfileDirectory::FLAG_ROOT))
				newShares[curPath] = dir;

			buildTree(curPath, dir, checkQueued, aSubRoots, aDirs, newShares);
		} else {
			// Not a directory, assume it's a file...
			string path = aPath + name;
			int64_t size = i->getSize();

			if (!checkSharedName(path, false, true, size)) {
				continue;
			}

			try {
				if(HashManager::getInstance()->checkTTH(path, size, i->getLastWriteTime())) 
					lastFileIter = aDir->files.insert(lastFileIter, Directory::File(name, size, aDir, HashManager::getInstance()->getTTH(path, size)));
			} catch(const HashException&) {
			}
		}
	}
}

bool ShareManager::checkHidden(const string& aName) const {
	FileFindIter ff = FileFindIter(aName.substr(0, aName.size() - 1));

	if (ff != FileFindIter()) {
		return (BOOLSETTING(SHARE_HIDDEN) || !ff->isHidden());
	}

	return true;
}

uint32_t ShareManager::findLastWrite(const string& aName) const {
	FileFindIter ff = FileFindIter(aName.substr(0, aName.size() - 1));

	if (ff != FileFindIter()) {
		return ff->getLastWriteTime();
	}

	return 0;
}

void ShareManager::updateIndices(Directory& dir,  bool first /*false*/) {
	dir.size = 0;
	if (dir.getProfileDir() && dir.getProfileDir()->hasRoots()) {
		auto profiles = dir.getProfileDir()->getShareProfiles();
		for(auto k = profiles.begin(); k != profiles.end(); ++k) {
			bloom.add(Text::toLower(k->second));
		}
	} else {
		bloom.add(Text::toLower(dir.getRealName()));
	}

	for(auto i = dir.directories.begin(); i != dir.directories.end(); ++i) {
		updateIndices(*i->second, false);
	}

	for(auto i = dir.files.begin(); i != dir.files.end(); ) {
		updateIndices(dir, i++);
	}
}

void ShareManager::rebuildIndices() {
	sharedSize = 0;
	bloom.clear();
	tthIndex.clear();

	DirMap parents;
	getParents(parents);
	for(auto i = parents.begin(); i != parents.end(); ++i) {
		updateIndices(*i->second);
	}
}

void ShareManager::updateIndices(Directory& dir, const Directory::File::Set::iterator& i) {
	const Directory::File& f = *i;
	dir.size += f.getSize();
	sharedSize += f.getSize();

	dir.addType(getType(f.getName()));

	tthIndex.insert(make_pair(const_cast<TTHValue*>(&f.getTTH()), i));
	bloom.add(Text::toLower(f.getName()));
}

int ShareManager::refresh(const string& aDir){
	int result = REFRESH_PATH_NOT_FOUND;
	string path = aDir;

	if(path[ path.length() -1 ] != PATH_SEPARATOR)
		path += PATH_SEPARATOR;

	StringList refreshPaths;

	{
		RLock l(cs);
		auto i = shares.find(path); //case insensitive
		if(i == shares.end()) {
			//loopup the Virtualname selected from share and add it to refreshPaths List
			for(auto j = shares.begin(); j != shares.end(); ++j) {
				auto& profiles = j->second->getProfileDir()->getShareProfiles();
				for(auto k = profiles.begin(); k != profiles.end(); ++k) {
					if(stricmp(k->second, aDir ) == 0 ) {
						refreshPaths.push_back(j->first);
						result = REFRESH_STARTED;
					}
				}
			}
		} else {
			refreshPaths.push_back(path);
			result = REFRESH_STARTED;
		}
	}

	if(result == REFRESH_PATH_NOT_FOUND)
		refreshing.clear();

	{
		WLock l (dirNames);
		tasks.add(REFRESH_DIR, unique_ptr<Task>(new StringListTask(refreshPaths)));
	}

	if(refreshing.test_and_set()) {
		LogManager::getInstance()->message(STRING(FILE_LIST_REFRESH_IN_PROGRESS), LogManager::LOG_INFO);
		return REFRESH_IN_PROGRESS;
	}

	if(result == REFRESH_STARTED)
		result = initTaskThread();

	return result;
}


int ShareManager::refresh(bool incoming /*false*/, bool isStartup /*false*/){
	if(refreshing.test_and_set()) {
		LogManager::getInstance()->message(STRING(FILE_LIST_REFRESH_IN_PROGRESS), LogManager::LOG_INFO);
		return REFRESH_IN_PROGRESS;
	}

	StringList dirs;

	{
		DirMap parents;
		{
			RLock l (cs);
			getParents(parents);
		}

		//if (incoming) {
			for(auto i = parents.begin(); i != parents.end(); ++i) {
				if (incoming && !i->second->getProfileDir()->isSet(ProfileDirectory::FLAG_INCOMING))
					continue;
				dirs.push_back(i->first);
			}
		//}
	}

	if (dirs.empty()){
		refreshing.clear();
		return REFRESH_PATH_NOT_FOUND;
	}

	{
		WLock l (dirNames);
		tasks.add(incoming ? REFRESH_INCOMING : REFRESH_ALL, unique_ptr<Task>(new StringListTask(dirs)));
	}

	initTaskThread(isStartup);
	return REFRESH_STARTED;
}

int ShareManager::initTaskThread(bool isStartup)  {
	join();
	try {
		start();
		if(isStartup) { 
			join();
		} else {
			setThreadPriority(Thread::NORMAL);
		}

	} catch(const ThreadException& e) {
		LogManager::getInstance()->message(STRING(FILE_LIST_REFRESH_FAILED) + " " + e.getError(), LogManager::LOG_WARNING);
		refreshing.clear();
	}

	return REFRESH_STARTED;
}

void ShareManager::getParents(DirMap& aDirs) const {
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		if (find_if(shares.begin(), shares.end(), [i](pair<string, Directory::Ptr> dir) { return AirUtil::isSub(i->first, dir.first); } ) == shares.end())
			aDirs.insert(*i);
	}
}

void ShareManager::getParentPaths(StringList& aDirs) const {
	RLock l (cs);
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		if (find_if(shares.begin(), shares.end(), [i](pair<string, Directory::Ptr> dir) { return AirUtil::isSub(i->first, dir.first); } ) == shares.end())
			aDirs.push_back(i->first);
	}
}

ShareManager::ProfileDirMap ShareManager::getSubProfileDirs(const string& aPath) {
	ProfileDirMap aRoots;
	for(auto i = profileDirs.begin(); i != profileDirs.end(); ++i) {
		if (AirUtil::isSub(i->first, aPath)) {
			aRoots[i->second->getPath()] = i->second;
		}
	}

	return aRoots;
}

void ShareManager::addProfiles(const ShareProfile::set& aProfiles) {
	WLock l (cs);
	for(auto i = aProfiles.begin(); i != aProfiles.end(); ++i) {
		shareProfiles.insert(shareProfiles.end()-1, *i);
	}
}

void ShareManager::removeProfiles(ProfileTokenList aProfiles) {
	WLock l (cs);
	boost::for_each(aProfiles, [this](ProfileToken aProfile) { shareProfiles.erase(remove(shareProfiles.begin(), shareProfiles.end(), aProfile), shareProfiles.end()); });
}

void ShareManager::addDirectories(const ShareDirInfo::list& aNewDirs) {
	StringList add;
	ProfileTokenSet profiles;

	{
		WLock l (cs);
		for(auto p = aNewDirs.begin(); p != aNewDirs.end(); ++p) {
			ShareDirInfo* d = *p;
			auto i = shares.find(d->path);
			if (i != shares.end()) {
				// Trying to share an already shared root
				i->second->getProfileDir()->addRootProfile(d->vname, d->profile);
				profiles.insert(d->profile);
			} else if (find_if(shares.begin(), shares.end(), [d](pair<string, Directory::Ptr> sdp) { return AirUtil::isSub(d->path, sdp.first); }) != shares.end()) {
				// It's a subdir
				auto dir = findDirectory(d->path, false, false);
				if (dir) {
					if (dir->getProfileDir()) {
						dir->getProfileDir()->addRootProfile(d->vname, d->profile);
					} else {
						auto root = ProfileDirectory::Ptr(new ProfileDirectory(d->path, d->vname, d->profile));
						dir->setProfileDir(root);
						profileDirs[d->path] = root;
					}
					shares[d->path] = dir;
					profiles.insert(d->profile);
				} else {
					//TODO, check excludes
				}
			} else {
				// It's a new parent
				auto root = ProfileDirectory::Ptr(new ProfileDirectory(d->path, d->vname, d->profile));
				//root->setFlag(ProfileDirectory::FLAG_ADD);
				Directory::Ptr dp = Directory::create(Util::getLastDir(d->path), nullptr, findLastWrite(d->path), root);
				shares[d->path] = dp;
				add.push_back(d->path);
			}
		}
	}

	if (add.empty()) {
		boost::for_each(profiles, [this](ProfileToken aProfile) { setDirty(aProfile); });
		return;
	}

	{
		WLock l (dirNames);
		tasks.add(ADD_DIR, unique_ptr<Task>(new StringListTask(add)));
	}

	initTaskThread();
}

void ShareManager::removeDirectories(const ShareDirInfo::list& aRemoveDirs) {
	ProfileTokenSet dirtyProfiles;
	bool rebuildIncides = false;

	{
		WLock l (cs);
		for(auto i = aRemoveDirs.begin(); i != aRemoveDirs.end(); ++i) {
			auto k = shares.find((*i)->path);
			if (k != shares.end()) {
				dirtyProfiles.insert((*i)->profile);

				if (k->second->getProfileDir()->removeRootProfile((*i)->profile)) {
					//dcassert(shareDirs.find(Util::getLastDir(i->path)) != shareDirs.end());
					//no other roots in here
					bool hasParent = k->second->getParent() != nullptr;
					if (!hasParent)
						removeDir(k->second);

					if (!k->second->getProfileDir()->hasExcludes()) {
						//delete k->second->getProfileDir();
						k->second->setProfileDir(nullptr);
						profileDirs.erase((*i)->path);
					}

					shares.erase((*i)->path);
					if (hasParent) {
						continue;
					}

					dcassert(dirNameMap.find(Util::getLastDir((*i)->path)) == dirNameMap.end());
					//no profiles in the parent, check if we have any child roots for other profiles inside this tree and get the most top one
					Directory::Ptr dir = nullptr;
					for(auto p = shares.begin(); p != shares.end(); ++p) {
						if(strnicmp((*i)->path, p->first, (*i)->path.length()) == 0 && (!dir || p->first.length() < dir->getProfileDir()->getPath().length())) {
							dir = p->second;
						}
					}

					if (dir) {
						dir->setParent(nullptr);
					}

					rebuildIncides = true;
				}
			}
		}
		rebuildIndices();
	}

	boost::for_each(dirtyProfiles, [this](ProfileToken aProfile) { setDirty(aProfile); });
}

void ShareManager::changeDirectories(const ShareDirInfo::list& renameDirs)  {
	ProfileTokenSet dirtyProfiles;
	for(auto i = renameDirs.begin(); i != renameDirs.end(); ++i) {
		string vName = validateVirtual((*i)->vname);
		dirtyProfiles.insert((*i)->profile);

		WLock l(cs);
		auto p = shares.find((*i)->path);
		if (p != shares.end()) {
			p->second->getProfileDir()->addRootProfile(vName, (*i)->profile); //renames it really
			auto pd = p->second->getProfileDir();
			(*i)->incoming ? p->second->getProfileDir()->setFlag(ProfileDirectory::FLAG_INCOMING) : p->second->getProfileDir()->unsetFlag(ProfileDirectory::FLAG_INCOMING);
		}
	}

	boost::for_each(dirtyProfiles, [this](ProfileToken aProfile) { setDirty(aProfile); });
}

void ShareManager::reportTaskStatus(uint8_t aTask, const StringList& directories, bool finished) {
	string msg;
	switch (aTask) {
		case(REFRESH_ALL):
			LogManager::getInstance()->message(finished ? STRING(FILE_LIST_REFRESH_FINISHED) : STRING(FILE_LIST_REFRESH_INITIATED), LogManager::LOG_INFO);
			break;
		case(REFRESH_DIR):
			if (directories.size() == 1) {
				msg = finished ? STRING_F(DIRECTORY_REFRESHED, *directories.begin()) : STRING_F(FILE_LIST_REFRESH_INITIATED_RPATH, *directories.begin());
			} else {
				if(boost::find_if(directories, [directories](const string& d) { return d != *directories.begin(); }) == directories.end()) {
					msg = finished ? STRING_F(VIRTUAL_DIRECTORY_REFRESHED, *directories.begin()) : STRING_F(FILE_LIST_REFRESH_INITIATED_RPATH, *directories.begin());
				} else {
					msg = finished ? STRING_F(X_DIRECTORIES_REFRESHED, directories.size()) : STRING_F(FILE_LIST_REFRESH_INITIATED_X_RPATH, directories.size());
				}
			}
			break;
		case(ADD_DIR):
			if (directories.size() == 1) {
				msg = finished ? STRING_F(DIRECTORY_ADDED, *directories.begin()) : STRING_F(ADDING_SHARED_DIR, *directories.begin());
			} else {
				msg = finished ? STRING_F(ADDING_X_SHARED_DIRS, directories.size()) : STRING_F(DIRECTORIES_ADDED, directories.size());
			}
			break;
		case(REFRESH_INCOMING):
			msg = finished ? STRING(FILE_LIST_REFRESH_INITIATED_INCOMING) : STRING(INCOMING_REFRESHED);
			break;
	};

	if (!msg.empty())
		LogManager::getInstance()->message(msg, LogManager::LOG_INFO);
}

int ShareManager::run() {
	HashManager::HashPauser pauser;

	for (;;) {
		TaskQueue::TaskPair t;
		if (!tasks.getFront(t))
			break;

		vector<pair<string, pair<Directory::Ptr, ProfileDirMap>>> dirs;
		auto directories = static_cast<StringListTask*>(t.second.get())->spl;
		for(auto i = directories.begin(); i != directories.end(); ++i) {
			RLock l (cs);
			auto d = shares.find(*i);
			if (d != shares.end()) {
				auto spd = getSubProfileDirs(*i);
				dirs.push_back(make_pair(*i, make_pair(d->second, spd)));
			}
		}

		reportTaskStatus(t.first, directories, false);
		if (t.first == REFRESH_INCOMING) {
			refreshRunning = true;
			lastIncomingUpdate = GET_TICK();
		} else if (t.first == REFRESH_ALL) {
			refreshRunning = true;
			lastFullUpdate = GET_TICK();
			lastIncomingUpdate = GET_TICK();
		}

		bundleDirs.clear();
		QueueManager::getInstance()->getForbiddenPaths(bundleDirs, directories);

		DirMultiMap newShareDirs;
		DirMap newShares;

		if(t.first == REFRESH_DIR || t.first == REFRESH_INCOMING || t.first == ADD_DIR) {
			{
				WLock l (cs);
				newShares = shares;
				for(auto i = dirs.begin(); i != dirs.end(); ++i) {
					removeDir(i->second.first);
				}
			}

			for(auto i = dirs.begin(); i != dirs.end(); ++i) {
				auto m = find_if(newShares.begin(), newShares.end(), [i](pair<string, Directory::Ptr> dir) { return AirUtil::isSub(dir.first, i->first); });
				if(m != newShares.end()) {
					newShares.erase(m);
				}
			}

			/*for(auto i = dirs.begin(); i != dirs.end(); ++i) {
				for(auto p = newShares.begin(); p != newShares.end();) {
					if (AirUtil::isSub(p->first, i->first))
						newShares.erase(p);
					else
						p++;
				}
			}*/
		}

		for(auto i = dirs.begin(); i != dirs.end(); ++i) {
			if (checkHidden(i->first)) {
				Directory::Ptr dp = Directory::create(Util::getLastDir(i->first), nullptr, findLastWrite(i->first), i->second.first->getProfileDir());
				newShareDirs.insert(make_pair(Util::getLastDir(i->first), dp));
				newShares[i->first] = dp;
				buildTree(i->first, dp, true, i->second.second, newShareDirs, newShares);
				if(aShutdown) goto end;  //abort refresh
			}
		}

		{		
			WLock l(cs);
			shares = newShares;
			if(t.first == REFRESH_DIR || t.first == REFRESH_INCOMING || t.first == ADD_DIR) {
				dirNameMap.insert(newShareDirs.begin(), newShareDirs.end());
			} else {
				dirNameMap = newShareDirs;
			}
				
			rebuildIndices();
			setDirty(true);  //forceXmlRefresh
		}
			
		if (t.first == REFRESH_STARTUP) {
			generateXmlList(SP_DEFAULT, true);
			saveXmlList();
		} else {
			ClientManager::getInstance()->infoUpdated();
		}

		reportTaskStatus(t.first, directories, true);
	}
end:
	{
		WLock l (dirNames);
		bundleDirs.clear();
	}
	refreshRunning = false;
	refreshing.clear();
	return 0;
}

void ShareManager::on(TimerManagerListener::Minute, uint64_t tick) noexcept {

	if(SETTING(SHARE_SAVE_TIME) > 0 && ShareCacheDirty && lastSave + SETTING(SHARE_SAVE_TIME) *60 *1000 <= tick) {
		saveXmlList();
	}

	if(SETTING(AUTO_REFRESH_TIME) > 0 && lastFullUpdate + SETTING(AUTO_REFRESH_TIME) * 60 * 1000 <= tick) {
		lastIncomingUpdate = tick;
		lastFullUpdate = tick;
		refresh(false);
	} else if(SETTING(INCOMING_REFRESH_TIME) > 0 && lastIncomingUpdate + SETTING(INCOMING_REFRESH_TIME) * 60 * 1000 <= tick) {
		lastIncomingUpdate = tick;
		refresh(true);
	}
}

void ShareManager::getShares(ShareDirInfo::map& aDirs) {
	RLock l (cs);
	for(auto i = shares.begin(); i != shares.end(); ++i) {
		auto profiles = i->second->getProfileDir()->getShareProfiles();
		for(auto p = profiles.begin(); p != profiles.end(); ++p) {
			auto sdi = new ShareDirInfo(p->second, p->first, i->first, i->second->getProfileDir()->isSet(ProfileDirectory::FLAG_INCOMING));
			sdi->size = i->second->getSize(p->first);
			aDirs[p->first].push_back(sdi);
		}
	}

}
		
void ShareManager::getBloom(ByteVector& v, size_t k, size_t m, size_t h) const {
	dcdebug("Creating bloom filter, k=%u, m=%u, h=%u\n", k, m, h);
	WLock l(cs);
	
	HashBloom bloom;
	bloom.reset(k, m, h);
	for(auto i = tthIndex.begin(); i != tthIndex.end(); ++i) {
		bloom.add(*i->first);
	}
	bloom.copy_to(v);
}

string ShareManager::generateOwnList(ProfileToken aProfile) {
	FileList* fl = generateXmlList(aProfile, true);
	return fl->getFileName();
}


//forwards the calls to createFileList for creating the filelist that was reguested.
FileList* ShareManager::generateXmlList(ProfileToken aProfile, bool forced /*false*/) {
	FileList* fl = nullptr;

	{
		WLock l(cs);
		auto i = find(shareProfiles.begin(), shareProfiles.end(), aProfile);
		if(i == shareProfiles.end()) {
			throw ShareException(UserConnection::FILE_NOT_AVAILABLE);
		}

		fl = (*i)->getProfileList() ? (*i)->getProfileList() : (*i)->generateProfileList();
	}

	createFileList(aProfile, fl, forced);
	return fl;
}

void ShareManager::createFileList(ProfileToken aProfile, FileList* fl, bool forced) {
	
	if(fl->isDirty(forced)) {
		fl->increaseN();

		try {
			SimpleXML xml;
			xml.addTag("FileListing");
			xml.addChildAttrib("Version", 1);
			xml.addChildAttrib("CID", ClientManager::getInstance()->getMe()->getCID().toBase32());
			xml.addChildAttrib("Base", string("/"));
			xml.addChildAttrib("Generator", string(APPNAME " " VERSIONSTRING));
			xml.stepIn();
			{
				RLock l(cs);
				for(auto i = shares.begin(); i != shares.end(); ++i) {
					if(i->second->getProfileDir()->hasProfile(aProfile))
						i->second->toXml(xml, true, aProfile);
				}
			}
			xml.stepOut();

			fl->saveList(xml);
		} catch(const Exception&) {
			// No new file lists...
		}
		fl->unsetDirty();
	}
}

#define LITERAL(n) n, sizeof(n)-1

void ShareManager::saveXmlList(bool verbose /* false */) {

	if(xml_saving)
		return;

	xml_saving = true;

	string indent;
	try {
		//create a backup first incase we get interrupted on creation.
		string newCache = Util::getPath(Util::PATH_USER_CONFIG) + "Shares.xml.tmp";
		File ff(newCache, File::WRITE, File::TRUNCATE | File::CREATE);
		BufferedOutputStream<false> xmlFile(&ff);
	
		xmlFile.write(SimpleXML::utf8Header);
		xmlFile.write(LITERAL("<Share Version=\"" SHARE_CACHE_VERSION "\">\r\n"));
		indent +='\t';

		{
			RLock l(cs);
			for(auto i = shares.begin(); i != shares.end(); ++i) {
				i->second->toXmlList(xmlFile, i->first, indent);
			}
		}

		xmlFile.write(LITERAL("</Share>"));
		xmlFile.flush();
		ff.close();
		File::deleteFile(Util::getPath(Util::PATH_USER_CONFIG) + "Shares.xml");
		File::renameFile(newCache,  (Util::getPath(Util::PATH_USER_CONFIG) + "Shares.xml"));
	}catch(Exception& e){
		LogManager::getInstance()->message("Error Saving Shares.xml: " + e.getError(), LogManager::LOG_WARNING);
	}

	//delete xmlFile;
	xml_saving = false;
	ShareCacheDirty = false;
	lastSave = GET_TICK();
	if (verbose)
		LogManager::getInstance()->message("shares.xml saved.", LogManager::LOG_INFO);
}

void ShareManager::Directory::toXmlList(OutputStream& xmlFile, const string& path, string& indent){
	string tmp, tmp2;
	
	xmlFile.write(indent);
	xmlFile.write(LITERAL("<Directory Name=\""));
	xmlFile.write(SimpleXML::escape(realName, tmp, true));
	xmlFile.write(LITERAL("\" Path=\""));
	xmlFile.write(SimpleXML::escape(path, tmp, true));
	xmlFile.write(LITERAL("\" Date=\""));
	xmlFile.write(SimpleXML::escape(Util::toString(lastWrite), tmp, true));
	xmlFile.write(LITERAL("\">\r\n"));

	indent += '\t';
	for(auto i = directories.begin(); i != directories.end(); ++i) {
		i->second->toXmlList(xmlFile, path + i->first + PATH_SEPARATOR, indent);
	}

	for(auto i = files.begin(); i != files.end(); ++i) {
		const Directory::File& f = *i;

		xmlFile.write(indent);
		xmlFile.write(LITERAL("<File Name=\""));
		xmlFile.write(SimpleXML::escape(f.getName(), tmp2, true));
		xmlFile.write(LITERAL("\" Size=\""));
		xmlFile.write(Util::toString(f.getSize()));
		xmlFile.write(LITERAL("\"/>\r\n"));
	}

	indent.erase(indent.length()-1);
	xmlFile.write(indent);
	xmlFile.write(LITERAL("</Directory>\r\n"));
}

MemoryInputStream* ShareManager::generateTTHList(const string& dir, bool recurse, ProfileToken aProfile) {
	
	if(aProfile == SP_HIDDEN)
		return NULL;
	
	string tths;
	string tmp;
	StringOutputStream sos(tths);

	try{
		RLock l(cs);
		DirectoryList result;
		findVirtuals(dir, aProfile, result); 
		for(auto it = result.begin(); it != result.end(); ++it) {
			dcdebug("result name %s \n", (*it)->getProfileDir()->getName(aProfile));
			(*it)->toTTHList(sos, tmp, recurse);
		}
	} catch(...) {
		return NULL;
	}

	if (tths.size() == 0) {
		dcdebug("Partial NULL");
		return NULL;
	} else {
		//LogManager::getInstance()->message(tths);
		return new MemoryInputStream(tths);
	}
}

void ShareManager::Directory::toTTHList(OutputStream& tthList, string& tmp2, bool recursive) {
	dcdebug("toTTHList2");
	if (recursive) {
		for(auto i = directories.begin(); i != directories.end(); ++i) {
			i->second->toTTHList(tthList, tmp2, recursive);
		}
	}
	for(auto i = files.begin(); i != files.end(); ++i) {
		const Directory::File& f = *i;
		tmp2.clear();
		tthList.write(f.getTTH().toBase32(tmp2));
		tthList.write(LITERAL(" "));
	}
}

MemoryInputStream* ShareManager::generatePartialList(const string& dir, bool recurse, ProfileToken aProfile) {
	if(dir[0] != '/' || dir[dir.size()-1] != '/')
		return 0;

	string xml;
	xml = SimpleXML::utf8Header;
	string basedate = Util::emptyString;

	SimpleXML sXml;   //use simpleXML so we can easily add the end tags and check what virtuals have been created.
	sXml.addTag("FileListing");
	sXml.addChildAttrib("Version", 1);
	sXml.addChildAttrib("CID", ClientManager::getInstance()->getMe()->getCID().toBase32());
	sXml.addChildAttrib("Base", dir);
	sXml.addChildAttrib("Generator", string(APPNAME " " VERSIONSTRING));
	sXml.stepIn();

	if(dir == "/") {
		RLock l(cs);
		for(auto i = shares.begin(); i != shares.end(); ++i) {
			if(i->first, i->second->getProfileDir()->hasProfile(aProfile))
				i->second->toXml(sXml, recurse, aProfile);
		}
	} else {
		dcdebug("wanted %s \n", dir);
		try {
			RLock l(cs);
			DirectoryList result;
			findVirtuals(dir, aProfile, result); 
			Directory::Ptr root;
			for(auto it = result.begin(); it != result.end(); ++it) {
				root = *it;
				dcdebug("result name %s \n", (*it)->getFullName(aProfile));

				if(basedate.empty() || (Util::toUInt32(basedate) < root->getLastWrite())) //compare the dates and add the last modified
					basedate = Util::toString(root->getLastWrite());
			
				for(auto it2 = root->directories.begin(); it2 != root->directories.end(); ++it2) {
					if (it2->second->isLevelExcluded(aProfile))
						continue;
					it2->second->toXml(sXml, recurse, aProfile);
				}
				root->filesToXml(sXml);
			}
		} catch(...) {
			return NULL;
		}
	}
	sXml.stepOut();
	sXml.addChildAttrib("BaseDate", basedate);

	StringOutputStream sos(xml);
	sXml.toXML(&sos);

	if (xml.empty()) {
		dcdebug("Partial NULL");
		return NULL;
	} else {
		return new MemoryInputStream(xml);
	}
}

void ShareManager::Directory::toXml(SimpleXML& xmlFile, bool fullList, ProfileToken aProfile) {
	bool create = true;

	xmlFile.resetCurrentChild();
	
	string vName = getVirtualName(aProfile);
	while( xmlFile.findChild("Directory") ){
		if( stricmp(xmlFile.getChildAttrib("Name"), vName) == 0 ){
			string curdate = xmlFile.getChildAttrib("Date");
			if(!curdate.empty() && Util::toUInt32(curdate) < lastWrite) //compare the dates and add the last modified
				xmlFile.replaceChildAttrib("Date", Util::toString(lastWrite));
			
			create = false;
			break;	
		}
	}

	if(create) {
		xmlFile.addTag("Directory");
		xmlFile.forceEndTag();
		xmlFile.addChildAttrib("Name", vName);
		xmlFile.addChildAttrib("Date", Util::toString(lastWrite));
	}

	if(fullList) {
		xmlFile.stepIn();
		for(auto i = directories.begin(); i != directories.end(); ++i) {
			if (i->second->isLevelExcluded(aProfile))
				continue;
			i->second->toXml(xmlFile, true, aProfile);
		}

		filesToXml(xmlFile);
		xmlFile.stepOut();
	} else {
		if((!directories.empty() || !files.empty())) {
			if(xmlFile.getChildAttrib("Incomplete").empty()) {
				xmlFile.addChildAttrib("Incomplete", 1);
			}
			int64_t size = Util::toInt64(xmlFile.getChildAttrib("Size"));
			xmlFile.replaceChildAttrib("Size", Util::toString(getSize(aProfile) + size));   //make the size accurate with virtuals, added a replace or add function to simpleXML
		}
	}
}

void ShareManager::Directory::filesToXml(SimpleXML& xmlFile) const {
	for(auto i = files.begin(); i != files.end(); ++i) {
		const Directory::File& f = *i;

		xmlFile.addTag("File");;
		xmlFile.addChildAttrib("Name", f.getName());
		xmlFile.addChildAttrib("Size", Util::toString(f.getSize()));
		xmlFile.addChildAttrib("TTH", f.getTTH().toBase32());
	}
}

// These ones we can look up as ints (4 bytes...)...

static const char* typeAudio[] = { ".mp3", ".mp2", ".mid", ".wav", ".ogg", ".wma", ".669", ".aac", ".aif", ".amf", ".ams", ".ape", ".dbm", ".dmf", ".dsm", ".far", ".mdl", ".med", ".mod", ".mol", ".mp1", ".mp4", ".mpa", ".mpc", ".mpp", ".mtm", ".nst", ".okt", ".psm", ".ptm", ".rmi", ".s3m", ".stm", ".ult", ".umx", ".wow" };
static const char* typeCompressed[] = { ".rar", ".zip", ".ace", ".arj", ".hqx", ".lha", ".sea", ".tar", ".tgz", ".uc2" };
static const char* typeDocument[] = { ".nfo", ".htm", ".doc", ".txt", ".pdf", ".chm" };
static const char* typeExecutable[] = { ".exe", ".com" };
static const char* typePicture[] = { ".jpg", ".gif", ".png", ".eps", ".img", ".pct", ".psp", ".pic", ".tif", ".rle", ".bmp", ".pcx", ".jpe", ".dcx", ".emf", ".ico", ".psd", ".tga", ".wmf", ".xif" };
static const char* typeVideo[] = { ".vob", ".mpg", ".mov", ".asf", ".avi", ".wmv", ".ogm", ".mkv", ".pxp", ".m1v", ".m2v", ".mpe", ".mps", ".mpv", ".ram" };

static const string type2Audio[] = { ".au", ".it", ".ra", ".xm", ".aiff", ".flac", ".midi", };
static const string type2Compressed[] = { ".gz" };
static const string type2Picture[] = { ".jpeg", ".ai", ".ps", ".pict", ".tiff" };
static const string type2Video[] = { ".mpeg", ".rm", ".divx", ".mp1v", ".mp2v", ".mpv1", ".mpv2", ".qt", ".rv", ".vivo" };

#define IS_TYPE(x) ( type == (*((uint32_t*)x)) )
#define IS_TYPE2(x) (stricmp(aString.c_str() + aString.length() - x.length(), x.c_str()) == 0) //hmm lower conversion...

bool ShareManager::checkType(const string& aString, int aType) {

	if(aType == SearchManager::TYPE_ANY)
		return true;

	if(aString.length() < 5)
		return false;
	
	const char* c = aString.c_str() + aString.length() - 3;
	if(!Text::isAscii(c))
		return false;

	uint32_t type = '.' | (Text::asciiToLower(c[0]) << 8) | (Text::asciiToLower(c[1]) << 16) | (((uint32_t)Text::asciiToLower(c[2])) << 24);

	switch(aType) {
	case SearchManager::TYPE_AUDIO:
		{
			for(size_t i = 0; i < (sizeof(typeAudio) / sizeof(typeAudio[0])); i++) {
				if(IS_TYPE(typeAudio[i])) {
					return true;
				}
			}
			for(size_t i = 0; i < (sizeof(type2Audio) / sizeof(type2Audio[0])); i++) {
				if(IS_TYPE2(type2Audio[i])) {
					return true;
				}
			}
		}
		break;
	case SearchManager::TYPE_COMPRESSED:
		{
			for(size_t i = 0; i < (sizeof(typeCompressed) / sizeof(typeCompressed[0])); i++) {
				if(IS_TYPE(typeCompressed[i])) {
					return true;
				}
			}
			if( IS_TYPE2(type2Compressed[0]) ) {
				return true;
			}
		}
		break;
	case SearchManager::TYPE_DOCUMENT:
		for(size_t i = 0; i < (sizeof(typeDocument) / sizeof(typeDocument[0])); i++) {
			if(IS_TYPE(typeDocument[i])) {
				return true;
			}
		}
		break;
	case SearchManager::TYPE_EXECUTABLE:
		if(IS_TYPE(typeExecutable[0]) || IS_TYPE(typeExecutable[1])) {
			return true;
		}
		break;
	case SearchManager::TYPE_PICTURE:
		{
			for(size_t i = 0; i < (sizeof(typePicture) / sizeof(typePicture[0])); i++) {
				if(IS_TYPE(typePicture[i])) {
					return true;
				}
			}
			for(size_t i = 0; i < (sizeof(type2Picture) / sizeof(type2Picture[0])); i++) {
				if(IS_TYPE2(type2Picture[i])) {
					return true;
				}
			}
		}
		break;
	case SearchManager::TYPE_VIDEO:
		{
			for(size_t i = 0; i < (sizeof(typeVideo) / sizeof(typeVideo[0])); i++) {
				if(IS_TYPE(typeVideo[i])) {
					return true;
				}
			}
			for(size_t i = 0; i < (sizeof(type2Video) / sizeof(type2Video[0])); i++) {
				if(IS_TYPE2(type2Video[i])) {
					return true;
				}
			}
		}
		break;
	default:
		dcassert(0);
		break;
	}
	return false;
}

SearchManager::TypeModes ShareManager::getType(const string& aFileName) noexcept {
	if(aFileName[aFileName.length() - 1] == PATH_SEPARATOR) {
		return SearchManager::TYPE_DIRECTORY;
	}
	 /*
	 optimize, check for compressed(rar) and audio first, the ones sharing the most are probobly sharing rars or mp3.
	 a test to match with regexp for rars first, otherwise it will match everything and end up setting type any for  .r01 ->
	 */
	try{ 
		if(RAR_regexp.match(aFileName, aFileName.length()-4) > 0)
			return SearchManager::TYPE_COMPRESSED;
	}catch(...) { } //not vital if it fails, just continue the type check.
	
	if(checkType(aFileName, SearchManager::TYPE_AUDIO))
		return SearchManager::TYPE_AUDIO;
	else if(checkType(aFileName, SearchManager::TYPE_VIDEO))
		return SearchManager::TYPE_VIDEO;
	else if(checkType(aFileName, SearchManager::TYPE_DOCUMENT))
		return SearchManager::TYPE_DOCUMENT;
	else if(checkType(aFileName, SearchManager::TYPE_COMPRESSED))
		return SearchManager::TYPE_COMPRESSED;
	else if(checkType(aFileName, SearchManager::TYPE_PICTURE))
		return SearchManager::TYPE_PICTURE;
	else if(checkType(aFileName, SearchManager::TYPE_EXECUTABLE))
		return SearchManager::TYPE_EXECUTABLE;

	return SearchManager::TYPE_ANY;
}

/**
 * Alright, the main point here is that when searching, a search string is most often found in 
 * the filename, not directory name, so we want to make that case faster. Also, we want to
 * avoid changing StringLists unless we absolutely have to --> this should only be done if a string
 * has been matched in the directory name. This new stringlist should also be used in all descendants,
 * but not the parents...
 */
void ShareManager::Directory::search(SearchResultList& aResults, StringSearch::List& aStrings, int aSearchType, int64_t aSize, int aFileType, StringList::size_type maxResults) const noexcept {
	// Skip everything if there's nothing to find here (doh! =)
	if(!hasType(aFileType))
		return;

	StringSearch::List* cur = &aStrings;
	unique_ptr<StringSearch::List> newStr;

	// Find any matches in the directory name
	for(auto k = aStrings.begin(); k != aStrings.end(); ++k) {
		if(k->match(profileDir ? profileDir->getName(SP_DEFAULT) : realName)) {
			if(!newStr.get()) {
				newStr = unique_ptr<StringSearch::List>(new StringSearch::List(aStrings));
			}
			newStr->erase(remove(newStr->begin(), newStr->end(), *k), newStr->end());
		}
	}

	if(newStr.get() != 0) {
		cur = newStr.get();
	}

	bool sizeOk = (aSearchType != SearchManager::SIZE_ATLEAST) || (aSize == 0);
	if( (cur->empty()) && 
		(((aFileType == SearchManager::TYPE_ANY) && sizeOk) || (aFileType == SearchManager::TYPE_DIRECTORY)) ) {
		// We satisfied all the search words! Add the directory...(NMDC searches don't support directory size)
		SearchResultPtr sr(new SearchResult(SearchResult::TYPE_DIRECTORY, 0, getFullName(SP_DEFAULT), TTHValue()));
		aResults.push_back(sr);
	}

	if(aFileType != SearchManager::TYPE_DIRECTORY) {
		for(auto i = files.begin(); i != files.end(); ++i) {
			
			if(aSearchType == SearchManager::SIZE_ATLEAST && aSize > i->getSize()) {
				continue;
			} else if(aSearchType == SearchManager::SIZE_ATMOST && aSize < i->getSize()) {
				continue;
			}

			auto j = cur->begin();
			for(; j != cur->end() && j->match(i->getName()); ++j) 
				;	// Empty
			
			if(j != cur->end())
				continue;
			
			// Check file type...
			if(checkType(i->getName(), aFileType)) {
				SearchResultPtr sr(new SearchResult(SearchResult::TYPE_FILE, i->getSize(), getFullName(SP_DEFAULT) + i->getName(), i->getTTH()));
				aResults.push_back(sr);
				if(aResults.size() >= maxResults) {
					break;
				}
			}
		}
	}

	for(auto l = directories.begin(); (l != directories.end()) && (aResults.size() < maxResults); ++l) {
		if (l->second->isLevelExcluded(SP_DEFAULT))
			continue;
		l->second->search(aResults, *cur, aSearchType, aSize, aFileType, maxResults);
	}
}
//NMDC Search
void ShareManager::search(SearchResultList& results, const string& aString, int aSearchType, int64_t aSize, int aFileType, StringList::size_type maxResults) noexcept {
	if(aFileType == SearchManager::TYPE_TTH) {
		if(aString.compare(0, 4, "TTH:") == 0) {
			TTHValue tth(aString.substr(4));
			auto i = tthIndex.find(const_cast<TTHValue*>(&tth));
			if(i != tthIndex.end() && i->second->getParent()->hasProfile(SP_DEFAULT)) {
				SearchResultPtr sr(new SearchResult(SearchResult::TYPE_FILE, i->second->getSize(), 
					i->second->getParent()->getFullName(SP_DEFAULT) + i->second->getName(), i->second->getTTH()));

				results.push_back(sr);
			} //lookup in temp shares, nmdc too?
		}
		return;
	}
	StringTokenizer<string> t(Text::toLower(aString), '$');
	StringList& sl = t.getTokens();
	allSearches++;
	if(!bloom.match(sl)) {
		stoppedSearches++;
		return;
	}

	StringSearch::List ssl;
	for(auto i = sl.begin(); i != sl.end(); ++i) {
		if(!i->empty()) {
			ssl.push_back(StringSearch(*i));
		}
	}
	if(ssl.empty())
		return;

	for(auto j = shares.begin(); (j != shares.end()) && (results.size() < maxResults); ++j) {
		if(j->second->getProfileDir()->hasProfile(SP_DEFAULT))
			j->second->search(results, ssl, aSearchType, aSize, aFileType, maxResults);
	}
}

string ShareManager::getBloomStats() {
	string ret = "Total StringSearches: " + Util::toString(allSearches) + ", stopped " + Util::toString((stoppedSearches > 0) ? (((double)stoppedSearches / (double)allSearches)*100) : 0) + " % (" + Util::toString(stoppedSearches) + " searches)";
	//ret += "Bloom size: " + Util::toString(bloom.getSize()) + ", length " + Util::toString(bloom.getLength());
	return ret;
}

/* Each matching directory is only being added once in the results. For directory results we return the path of the parent directory and for files the current directory */
void ShareManager::Directory::directSearch(DirectSearchResultList& aResults, AdcSearch& aStrings, StringList::size_type maxResults, ProfileToken aProfile) const noexcept {
	if(aStrings.matchesDirectDirectoryName(profileDir ? profileDir->getName(aProfile) : realName)) {
		auto path = parent ? parent->getADCPath(aProfile) : "/";
		auto res = boost::find_if(aResults, [path](DirectSearchResultPtr sr) { return sr->getPath() == path; });
		if (res == aResults.end() && aStrings.matchesSize(getSize(aProfile))) {
			DirectSearchResultPtr sr(new DirectSearchResult(path));
			aResults.push_back(sr);
		}
	}

	if(!aStrings.isDirectory) {
		for(auto i = files.begin(); i != files.end(); ++i) {
			if(aStrings.matchesDirectFile((*i).getName(), (*i).getSize())) {
				DirectSearchResultPtr sr(new DirectSearchResult(getADCPath(aProfile)));
				aResults.push_back(sr);
				break;
			}
		}
	}


	for(auto l = directories.begin(); (l != directories.end()) && (aResults.size() < maxResults); ++l) {
		if (l->second->isLevelExcluded(aProfile))
			continue;
		l->second->directSearch(aResults, aStrings, maxResults, aProfile);
	}
}

void ShareManager::directSearch(DirectSearchResultList& results, AdcSearch& srch, StringList::size_type maxResults, ProfileToken aProfile, const string& aDirectory) noexcept {
	RLock l(cs);
	if(srch.hasRoot) {
		auto flst = tthIndex.equal_range(const_cast<TTHValue*>(&srch.root));
		for(auto f = flst.first; f != flst.second; ++f) {
			if (f->second->getParent()->hasProfile(aProfile)) {
				DirectSearchResultPtr sr(new DirectSearchResult(f->second->getParent()->getADCPath(aProfile)));
				results.push_back(sr);
			}
		}
		return;
	}

	for(auto i = srch.includeX.begin(); i != srch.includeX.end(); ++i) {
		if(!bloom.match(i->getPattern())) {
			return;
		}
	}

	/*for(auto j = dirNameMap.begin(); (j != dirNameMap.end()) && (results.size() < maxResults); ++j) {
		if(j->second->getProfileDir()->hasProfile(aProfile))
			j->second->directSearch(results, srch, maxResults, aProfile);
	}*/

	if (aDirectory.empty() || aDirectory == "/") {
		for(auto j = shares.begin(); (j != shares.end()) && (results.size() < maxResults); ++j) {
			if(j->second->getProfileDir()->hasProfile(aProfile))
				j->second->directSearch(results, srch, maxResults, aProfile);
		}
	} else {
		DirectoryList result;
		findVirtuals(aDirectory, aProfile, result); 
		Directory::Ptr root;
		for(auto it = result.begin(); it != result.end(); ++it) {
			if (!(*it)->isLevelExcluded(aProfile))
				(*it)->directSearch(results, srch, maxResults, aProfile);
		}
	}
}

void ShareManager::Directory::search(SearchResultList& aResults, AdcSearch& aStrings, StringList::size_type maxResults, ProfileToken aProfile) const noexcept {
	StringSearch::List* old = aStrings.include;

	unique_ptr<StringSearch::List> newStr;

	// Find any matches in the directory name
	for(auto k = aStrings.include->begin(); k != aStrings.include->end(); ++k) {
		if(k->match(profileDir ? profileDir->getName(aProfile) : realName) && !aStrings.isExcluded(profileDir ? profileDir->getName(aProfile) : realName)) {
			if(!newStr.get()) {
				newStr = unique_ptr<StringSearch::List>(new StringSearch::List(*aStrings.include));
			}
			newStr->erase(remove(newStr->begin(), newStr->end(), *k), newStr->end());
		}
	}

	if(newStr.get() != 0) {
		aStrings.include = newStr.get();
	}

	bool sizeOk = (aStrings.gt == 0);
	if( aStrings.include->empty() && aStrings.ext.empty() && sizeOk ) {
		// We satisfied all the search words! Add the directory...
		SearchResultPtr sr(new SearchResult(SearchResult::TYPE_DIRECTORY, getSize(aProfile), getFullName(aProfile), TTHValue()));
		aResults.push_back(sr);
	}

	if(!aStrings.isDirectory) {
		for(auto i = files.begin(); i != files.end(); ++i) {

			if(!(i->getSize() >= aStrings.gt)) {
				continue;
			} else if(!(i->getSize() <= aStrings.lt)) {
				continue;
			}	

			if(aStrings.isExcluded(i->getName()))
				continue;

			auto j = aStrings.include->begin();
			for(; j != aStrings.include->end() && j->match(i->getName()); ++j) 
				;	// Empty

			if(j != aStrings.include->end())
				continue;

			// Check file type...
			if(aStrings.hasExt(i->getName())) {

				SearchResultPtr sr(new SearchResult(SearchResult::TYPE_FILE, 
					i->getSize(), getFullName(aProfile) + i->getName(), i->getTTH()));
				aResults.push_back(sr);
				if(aResults.size() >= maxResults) {
					return;
				}
			}
		}
	}

	for(auto l = directories.begin(); (l != directories.end()) && (aResults.size() < maxResults); ++l) {
		if (l->second->isLevelExcluded(aProfile))
			continue;
		l->second->search(aResults, aStrings, maxResults, aProfile);
	}

	//faster to check this?
	if (aStrings.include->size() != old->size())
		aStrings.include = old;
}


void ShareManager::search(SearchResultList& results, const StringList& params, StringList::size_type maxResults, ProfileToken aProfile, const CID& cid) noexcept {

	AdcSearch srch(params);	

	RLock l(cs);

	if(srch.hasRoot) {
		auto i = tthIndex.find(const_cast<TTHValue*>(&srch.root));
		if(i != tthIndex.end() && i->second->getParent()->hasProfile(aProfile)) {
			SearchResultPtr sr(new SearchResult(SearchResult::TYPE_FILE, 
				i->second->getSize(), i->second->getParent()->getFullName(aProfile) + i->second->getName(), 
				i->second->getTTH()));
			results.push_back(sr);
			return;
		}

		Lock l(tScs);
		auto Files = tempShares.equal_range(srch.root);
		for(auto i = Files.first; i != Files.second; ++i) {
			if(i->second.key.empty() || (i->second.key == cid.toBase32())) { // if no key is set, it means its a hub share.
				SearchResultPtr sr(new SearchResult(SearchResult::TYPE_FILE, i->second.size, "tmp\\" + Util::getFileName(i->second.path), i->first));
				results.push_back(sr);
			}
		}
		return;
	}

	allSearches++;
	for(auto i = srch.includeX.begin(); i != srch.includeX.end(); ++i) {
		if(!bloom.match(i->getPattern())) {
			stoppedSearches++;
			return;
		}
	}

	for(auto j = shares.begin(); (j != shares.end()) && (results.size() < maxResults); ++j) {
		if(j->second->getProfileDir()->hasProfile(aProfile))
			j->second->search(results, srch, maxResults, aProfile);
	}
}
void ShareManager::cleanIndices(Directory::Ptr& dir) {
	for(auto i = dir->directories.begin(); i != dir->directories.end(); ++i) {
		cleanIndices(i->second);
	}

	for(auto i = dir->files.begin(); i != dir->files.end(); ++i) {
		auto flst = tthIndex.equal_range(const_cast<TTHValue*>(&i->getTTH()));
		for(auto f = flst.first; f != flst.second; ++f) {
			if(stricmp(f->second->getRealPath(false), i->getRealPath(false)) == 0) {
				tthIndex.erase(f);
				break;
			}
		}
	}

	removeDir(dir);

	dir->files.clear();
	dir->directories.clear();
}

void ShareManager::on(QueueManagerListener::BundleAdded, const BundlePtr aBundle) noexcept {
	WLock l (dirNames);
	bundleDirs.insert(upper_bound(bundleDirs.begin(), bundleDirs.end(), aBundle->getTarget()), aBundle->getTarget());
}

void ShareManager::on(QueueManagerListener::BundleHashed, const string& path) noexcept {
	{
		WLock l(cs);
		Directory::Ptr dir = findDirectory(path, true, true);
		if (!dir) {
			LogManager::getInstance()->message(STRING_F(BUNDLE_SHARING_FAILED, Util::getLastDir(path).c_str()), LogManager::LOG_WARNING);
			return;
		}

		/* get rid of any existing crap we might have in the bundle directory and refresh it.
		done at this point as the file and directory pointers should still be valid, if there are any */
		cleanIndices(dir);

		ProfileDirMap profileDirs;
		DirMap newShares;
		buildTree(path, dir, false, profileDirs, dirNameMap, newShares);
		updateIndices(*dir);
		setDirty(true); //forceXmlRefresh
	}

	LogManager::getInstance()->message(STRING_F(BUNDLE_SHARED, path.c_str()), LogManager::LOG_INFO);
}

bool ShareManager::allowAddDir(const string& path) noexcept {
	//LogManager::getInstance()->message("QueueManagerListener::BundleFilesMoved");
	{
		RLock l(cs);
		auto mi = find_if(shares.begin(), shares.end(), [path](pair<string, Directory::Ptr> dp) { return AirUtil::isParentOrExact(dp.first, path); });
		if (mi != shares.end()) {
			//check the skiplist
			StringList sl = StringTokenizer<string>(path.substr(mi->first.length()), PATH_SEPARATOR).getTokens();
			string fullPath = mi->first;
			for(auto i = sl.begin(); i != sl.end(); ++i) {
				fullPath += Text::toLower(*i) + PATH_SEPARATOR;
				if (!checkSharedName(fullPath, true, true)) {
					return false;
				}

				auto m = profileDirs.find(fullPath);
				if (m != profileDirs.end() && m->second->isSet(ProfileDirectory::FLAG_EXCLUDE_TOTAL)) {
					return false;
				}
			}
			return true;
		}
	}
	return false;
}

ShareManager::Directory::Ptr ShareManager::findDirectory(const string& fname, bool allowAdd, bool report) {
	auto mi = find_if(shares.begin(), shares.end(), [fname](pair<string, Directory::Ptr> dp) { return AirUtil::isParentOrExact(dp.first, fname); });
	if (mi != shares.end()) {
		auto curDir = mi->second;
		StringList sl = StringTokenizer<string>(fname.substr(mi->first.length()), PATH_SEPARATOR).getTokens();
		string fullPath = Text::toLower(mi->first);
		for(auto i = sl.begin(); i != sl.end(); ++i) {
			fullPath += *i + PATH_SEPARATOR;
			auto j = curDir->directories.find(*i);
			if (j != curDir->directories.end()) {
				curDir = j->second;
			} else if (!allowAdd || !checkSharedName(fullPath, true, report)) {
				return nullptr;
			} else {
				auto m = profileDirs.find(fullPath);
				if (m != profileDirs.end() && m->second->isSet(ProfileDirectory::FLAG_EXCLUDE_TOTAL)) {
					return nullptr;
				}

				curDir = Directory::create(*i, curDir, GET_TIME(), m != profileDirs.end() ? m->second : nullptr);
				dirNameMap.insert(make_pair(*i, curDir));
			}
		}
		return curDir;
	}
	return nullptr;
}

void ShareManager::onFileHashed(const string& fname, const TTHValue& root) noexcept {
	WLock l(cs);
	Directory::Ptr d = findDirectory(Util::getFilePath(fname), true, false);
	if (!d) {
		return;
	}

	auto i = d->findFile(Util::getFileName(fname));
	if(i != d->files.end()) {
		// Get rid of false constness...
		auto files = tthIndex.equal_range(const_cast<TTHValue*>(&i->getTTH()));
		for(auto k = files.first; k != files.second; ++k) {
			if(stricmp(fname.c_str(), k->second->getRealPath(false).c_str()) == 0) {
				tthIndex.erase(k);
				break;
			}
		}

		Directory::File* f = const_cast<Directory::File*>(&(*i));
		f->setTTH(root);
		tthIndex.insert(make_pair(const_cast<TTHValue*>(&f->getTTH()), i));
	} else {
		string name = Util::getFileName(fname);
		int64_t size = File::getSize(fname);
		auto it = d->files.insert(Directory::File(name, size, d, root)).first;
		updateIndices(*d, it);
	}
		
	setDirty();
}

void ShareManager::getExcludes(ProfileToken aProfile, StringList& excludes) {
	for(auto i = profileDirs.begin(); i != profileDirs.end(); ++i) {
		if (i->second->isExcluded(aProfile))
			excludes.push_back(i->first);
	}
}

void ShareManager::changeExcludedDirs(const ProfileTokenStringSetMap& aAdd, const ProfileTokenStringSetMap& aRemove) {
	ProfileTokenSet dirtyProfiles;

	{
		WLock l (cs);
		for(auto i = aAdd.begin(); i != aAdd.end(); ++i) {
			for(auto j = i->second.begin(); j != i->second.end(); ++j) {
				ProfileDirectory::Ptr pd = nullptr;

				auto dir = findDirectory(*j, false, false);
				if (dir) {
					dirtyProfiles.insert(i->first);
					if (dir->getProfileDir()) {
						dir->getProfileDir()->addExclude(i->first);
						pd = dir->getProfileDir();
					} else {
						pd = ProfileDirectory::Ptr(new ProfileDirectory(*j, i->first));
						dir->setProfileDir(pd);
					}
				} else {
					pd = ProfileDirectory::Ptr(new ProfileDirectory(*j, i->first));
				}
				profileDirs[*j] = pd;
			}
		}

		for(auto i = aRemove.begin(); i != aRemove.end(); ++i) {
			for(auto j = i->second.begin(); j != i->second.end(); ++j) {
				boost::for_each(i->second, [&](const string& aPath) { profileDirs.erase(aPath); });
			}
		}
	}

	boost::for_each(dirtyProfiles, [this](ProfileToken aProfile) { setDirty(aProfile); });
	rebuildExcludeTypes();
}

void ShareManager::rebuildExcludeTypes() {
	RLock l (cs);
	for(auto i = profileDirs.begin(); i != profileDirs.end(); ++i) {
		if (!i->second->isSet(ProfileDirectory::FLAG_EXCLUDE_PROFILE))
			continue;

		i->second->unsetFlag(ProfileDirectory::FLAG_EXCLUDE_TOTAL);
		ProfileTokenSet shared;

		//List all profiles where this dir is shared in
		for(auto j = shares.begin(); j != shares.end(); ++j) {
			if (AirUtil::isParentOrExact(j->first, i->first)) {
				LogManager::getInstance()->message(j->first + " is the parent of " + i->first, LogManager::LOG_INFO);
				boost::for_each(j->second->getProfileDir()->getShareProfiles(), [&shared](pair<ProfileToken, string> ap) { shared.insert(ap.first); });
			}
		}

		if (!shared.empty()) {
			//Check excludes for the listed dirs
			for(auto j = profileDirs.begin(); j != profileDirs.end(); ++j) {
				if (i->second->isSet(ProfileDirectory::FLAG_EXCLUDE_PROFILE) && AirUtil::isParentOrExact(i->first, j->first)) {
					LogManager::getInstance()->message(i->first + " is the parent of " + j->first, LogManager::LOG_INFO);
					boost::for_each(j->second->getexcludedProfiles(), [&shared](ProfileToken ap) { shared.erase(ap); });
				}
			}
		}

		if (shared.empty()) {
			//LogManager::getInstance()->message(i->first + " is a total exclude", LogManager::LOG_INFO);
			i->second->setFlag(ProfileDirectory::FLAG_EXCLUDE_TOTAL);
		} else {
			//LogManager::getInstance()->message(i->first + " is NOT a total exclude", LogManager::LOG_INFO);
		}
	}
}

vector<pair<string, StringList>> ShareManager::getGroupedDirectories() const noexcept {
	vector<pair<string, StringList>> ret;
	DirMap parents;
	
	{
		RLock l (cs);
		getParents(parents);
		for(auto i = shares.begin(); i != shares.end(); ++i) {
			auto spl = i->second->getProfileDir()->getShareProfiles();
			for(auto p = spl.begin(); p != spl.end(); ++p) {
				auto retVirtual = find_if(ret.begin(), ret.end(), CompareFirst<string, StringList>(p->second));
				if (retVirtual != ret.end()) {
					if (find(retVirtual->second.begin(), retVirtual->second.end(), i->first) == retVirtual->second.end())
						retVirtual->second.insert(upper_bound(retVirtual->second.begin(), retVirtual->second.end(), i->first), i->first);
				} else {
					StringList tmp;
					tmp.push_back(i->first);
					ret.push_back(make_pair(p->second, tmp));
				}
			}
		}
	}

	sort(ret.begin(), ret.end());
	return ret;
}

bool ShareManager::checkSharedName(const string& aPath, bool isDir, bool report /*true*/, int64_t size /*0*/) {
	string aName;
	aName = isDir ? Util::getLastDir(aPath) : Util::getFileName(aPath);

	if(aName == "." || aName == "..")
		return false;

	if (skipList.match(aName)) {
		if(BOOLSETTING(REPORT_SKIPLIST) && report)
			LogManager::getInstance()->message("Share Skiplist blocked file, not shared: " + aPath /*+ " (" + STRING(DIRECTORY) + ": \"" + aName + "\")"*/, LogManager::LOG_INFO);
		return false;
	}

	aName = Text::toLower(aName); //we only need this now
	if (!isDir) {
		string fileExt = Util::getFileExt(aName);
		if( (strcmp(aName.c_str(), "dcplusplus.xml") == 0) || 
			(strcmp(aName.c_str(), "favorites.xml") == 0) ||
			(strcmp(fileExt.c_str(), ".dctmp") == 0) ||
			(strcmp(fileExt.c_str(), ".antifrag") == 0) ) 
		{
			return false;
		}

		//check for forbidden file patterns
		if(BOOLSETTING(REMOVE_FORBIDDEN)) {
			string::size_type nameLen = aName.size();
			if ((strcmp(fileExt.c_str(), ".tdc") == 0) ||
				(strcmp(fileExt.c_str(), ".getright") == 0) ||
				(strcmp(fileExt.c_str(), ".temp") == 0) ||
				(strcmp(fileExt.c_str(), ".tmp") == 0) ||
				(strcmp(fileExt.c_str(), ".jc!") == 0) ||	//FlashGet
				(strcmp(fileExt.c_str(), ".dmf") == 0) ||	//Download Master
				(strcmp(fileExt.c_str(), ".!ut") == 0) ||	//uTorrent
				(strcmp(fileExt.c_str(), ".bc!") == 0) ||	//BitComet
				(strcmp(fileExt.c_str(), ".missing") == 0) ||
				(strcmp(fileExt.c_str(), ".bak") == 0) ||
				(strcmp(fileExt.c_str(), ".bad") == 0) ||
				(nameLen > 9 && aName.rfind("part.met") == nameLen - 8) ||				
				(aName.find("__padding_") == 0) ||			//BitComet padding
				(aName.find("__incomplete__") == 0)		//winmx
				) {
					if (report) {
						LogManager::getInstance()->message("Forbidden file will not be shared: " + aPath/* + " (" + STRING(DIRECTORY) + ": \"" + aName + "\")"*/, LogManager::LOG_INFO);
					}
					return false;
			}
		}

		if(Util::stricmp(aPath, privKeyFile) == 0) {
			return false;
		}

		if(BOOLSETTING(NO_ZERO_BYTE) && !(size > 0))
			return false;

		if ((SETTING(MAX_FILE_SIZE_SHARED) != 0) && (size > (SETTING(MAX_FILE_SIZE_SHARED)*1024*1024))) {
			if (report) {
				LogManager::getInstance()->message(STRING(BIG_FILE_NOT_SHARED) + " " + aPath, LogManager::LOG_INFO);
			}
			return false;
		}
	} else {
#ifdef _WIN32
		// don't share Windows directory
		if(aPath.length() >= winDir.length() && stricmp(aPath.substr(0, winDir.length()), winDir) == 0)
			return false;
#endif
		if((stricmp(aPath, tempDLDir) == 0)) {
			return false;
		}
	}
	return true;
}

void ShareManager::setSkipList() {
	skipList.pattern = SETTING(SKIPLIST_SHARE);
	skipList.setMethod(BOOLSETTING(SHARE_SKIPLIST_USE_REGEXP) ? StringMatch::REGEX : StringMatch::WILDCARD);
	skipList.prepare();
}

} // namespace dcpp

/**
 * @file
 * $Id: ShareManager.cpp 473 2010-01-12 23:17:33Z bigmuscle $
 */
