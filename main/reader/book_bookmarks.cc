#include "book_bookmarks.h"
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#endif
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <unistd.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#endif

namespace reader {
namespace {
const char* BookmarkText(const char* zh, const char* en) {
    #ifndef EBOOK_EMULATOR
    return std::strcmp(Lang::Current(), "zh-CN") == 0 ? zh : en;
    #else
    (void)zh;
    return en;
    #endif
}

using Bytes=std::vector<uint8_t>;
void Put32(Bytes& bytes,uint32_t value) { for(int i=0;i<4;++i) bytes.push_back(value>>(i*8)); }
void Put64(Bytes& bytes,uint64_t value) { Put32(bytes,value);Put32(bytes,value>>32); }
uint32_t Checksum(const Bytes& bytes,size_t length) {
    uint32_t hash=2166136261u;
    for(size_t i=0;i<length;++i) { hash^=bytes[i];hash*=16777619u; }
    return hash;
}
struct Cursor {
    const Bytes& bytes;size_t pos=4;bool ok=true;
    uint32_t U32() {
        if(pos+4>bytes.size()) { ok=false;return 0; }
        uint32_t value=0;for(int i=0;i<4;++i)value|=uint32_t(bytes[pos++])<<(i*8);return value;
    }
    uint64_t U64() { uint64_t low=U32();return low|(uint64_t(U32())<<32); }
    BookSourcePosition Position() { BookSourcePosition p;p.chapter=U32();p.block=U32();p.offset=U32();return p; }
};
bool Read(const std::string& path,uint64_t size,int64_t mtime,uint32_t format,
          std::vector<BookBookmark>& entries,std::string& error,bool& missing) {
    FILE* file=std::fopen(path.c_str(),"rb");
    if(!file) { missing=errno==ENOENT;error=BookmarkText("无法读取书签文件", "Cannot read bookmarks");return false; }
    missing=false;
    std::fseek(file,0,SEEK_END);const long length=std::ftell(file);std::rewind(file);
    if(length<36 || length>long(36+BookBookmarkStore::kMaxBookmarks*(44+BookBookmarkStore::kMaxExcerptBytes))) {
        std::fclose(file);error=BookmarkText("书签文件损坏", "Bookmark file is damaged");return false;
    }
    Bytes bytes;
    try { bytes.resize(length); } catch(...) { std::fclose(file);throw; }
    const bool read_ok=std::fread(bytes.data(),1,bytes.size(),file)==bytes.size();std::fclose(file);
    if(!read_ok || std::memcmp(bytes.data(),"BKM1",4)) { error=BookmarkText("书签文件损坏", "Bookmark file is damaged");return false; }
    Cursor crc{bytes};crc.pos=bytes.size()-4;
    if(crc.U32()!=Checksum(bytes,bytes.size()-4)) { error=BookmarkText("书签文件校验失败", "Bookmark verification failed");return false; }
    Cursor cursor{bytes};
    const uint32_t version=cursor.U32();const uint64_t saved_size=cursor.U64();
    const int64_t saved_mtime=static_cast<int64_t>(cursor.U64());const uint32_t saved_format=cursor.U32();
    const uint32_t count=cursor.U32();
    if(version!=1 || count>BookBookmarkStore::kMaxBookmarks) { error=BookmarkText("不支持的书签文件", "Unsupported bookmark file");return false; }
    if(saved_size!=size || saved_format!=format || (saved_mtime && mtime && saved_mtime!=mtime)) {
        error=BookmarkText("书籍已变更，原书签无法定位", "The book has changed. Original bookmarks cannot be located.");return false;
    }
    std::vector<BookBookmark> loaded;loaded.reserve(count);
    for(uint32_t i=0;i<count;++i) {
        BookBookmark entry;entry.id=cursor.U32();entry.start=cursor.Position();entry.end=cursor.Position();
        entry.page=cursor.U32();entry.added_at=static_cast<int64_t>(cursor.U64());const uint32_t len=cursor.U32();
        if(!cursor.ok || !entry.id || len>BookBookmarkStore::kMaxExcerptBytes || cursor.pos+len>bytes.size()-4 ||
           !SourceBefore(entry.start,entry.end) ||
           std::any_of(loaded.begin(),loaded.end(),[&](const BookBookmark& other){return other.id==entry.id;})) {
            error=BookmarkText("书签文件损坏", "Bookmark file is damaged");return false;
        }
        entry.excerpt.assign(reinterpret_cast<const char*>(bytes.data()+cursor.pos),len);cursor.pos+=len;
        loaded.push_back(std::move(entry));
    }
    if(!cursor.ok || cursor.pos!=bytes.size()-4) { error=BookmarkText("书签文件损坏", "Bookmark file is damaged");return false; }
    std::sort(loaded.begin(),loaded.end(),[](const BookBookmark& a,const BookBookmark& b){return a.id>b.id;});
    entries.swap(loaded);return true;
}
}
void BookBookmarkStore::Load(const std::string& book_path,uint64_t size,int64_t mtime,uint32_t format) {
    path_=book_path+".bmk";file_size_=size;mtime_=mtime;format_=format;entries_.clear();error_.clear();
    try {
        bool missing=false;
        if(Read(path_,size,mtime,format,entries_,error_,missing)) { error_.clear();return; }
        const std::string primary_error=error_;bool temp_missing=false;
        if(Read(path_+".tmp",size,mtime,format,entries_,error_,temp_missing)) {
            // A completed temporary survives interruption during FAT's rename.
            std::rename((path_+".tmp").c_str(),path_.c_str());error_.clear();return;
        }
        error_=missing&&temp_missing ? "" : missing ? error_ : primary_error;
    } catch(const std::bad_alloc&) { entries_.clear();error_=BookmarkText("内存不足，无法读取书签", "Not enough memory to read bookmarks"); }
}
bool BookBookmarkStore::Save(const std::vector<BookBookmark>& entries,std::string& error) {
    if(!error_.empty()) { error=error_;return false; }
    Bytes bytes{'B','K','M','1'};Put32(bytes,1);Put64(bytes,file_size_);Put64(bytes,mtime_);
    Put32(bytes,format_);Put32(bytes,entries.size());
    for(const auto& entry:entries) {
        Put32(bytes,entry.id);
        for(const auto& p:{entry.start,entry.end}) { Put32(bytes,p.chapter);Put32(bytes,p.block);Put32(bytes,p.offset); }
        Put32(bytes,entry.page);Put64(bytes,entry.added_at);Put32(bytes,entry.excerpt.size());
        bytes.insert(bytes.end(),entry.excerpt.begin(),entry.excerpt.end());
    }
    Put32(bytes,Checksum(bytes,bytes.size()));
    const std::string temporary=path_+".tmp";FILE* file=std::fopen(temporary.c_str(),"wb");
    if(!file) { error=BookmarkText("书签保存失败，请检查 SD 卡", "Cannot save bookmarks. Check the SD card.");return false; }
    bool ok=std::fwrite(bytes.data(),1,bytes.size(),file)==bytes.size();
    ok=std::fflush(file)==0 && ok;
#ifdef _WIN32
    if(ok && _commit(fileno(file))!=0)ok=false;
#else
    if(ok && fsync(fileno(file))!=0 && errno!=ENOSYS) ok=false;
#endif
    if(std::fclose(file)!=0) ok=false;
    if(!ok) { std::remove(temporary.c_str());error=BookmarkText("书签保存失败，请检查 SD 卡", "Cannot save bookmarks. Check the SD card.");return false; }
    int renamed=std::rename(temporary.c_str(),path_.c_str());
    if(renamed!=0 && (errno==EEXIST || errno==EACCES)) {
        // FAT/Windows cannot replace an existing destination with rename.
        // The complete, synced temporary remains recoverable across this gap.
        struct stat old{};
        if(stat(path_.c_str(),&old)==0 && S_ISREG(old.st_mode) && std::remove(path_.c_str())==0)
            renamed=std::rename(temporary.c_str(),path_.c_str());
    }
    if(renamed!=0) {
        error=BookmarkText("书签保存失败，请检查 SD 卡", "Cannot save bookmarks. Check the SD card.");return false;
    }
    return true;
}
bool BookBookmarkStore::Add(BookBookmark entry,std::string& error) {
    try {
        if(!SourceBefore(entry.start,entry.end) || entry.excerpt.size()>kMaxExcerptBytes) { error=BookmarkText("无有效正文位置", "No valid text position");return false; }
        if(entries_.size()>=kMaxBookmarks) { error=BookmarkText("书签已满（最多256条）", "Bookmark limit reached (256)");return false; }
        uint32_t last=0;for(const auto& other:entries_)last=std::max(last,other.id);
        if(last==std::numeric_limits<uint32_t>::max()) { error=BookmarkText("书签编号已满", "No more bookmark IDs available");return false; }
        entry.id=last+1;auto updated=entries_;updated.push_back(std::move(entry));
        std::sort(updated.begin(),updated.end(),[](const BookBookmark& a,const BookBookmark& b){return a.id>b.id;});
        if(!Save(updated,error))return false;
        entries_.swap(updated);return true;
    } catch(const std::bad_alloc&) { error=BookmarkText("内存不足，无法保存书签", "Not enough memory to save bookmarks");return false; }
}
bool BookBookmarkStore::Remove(const std::vector<uint32_t>& ids,std::string& error) {
    try {
        auto updated=entries_;
        updated.erase(std::remove_if(updated.begin(),updated.end(),[&](const BookBookmark& entry){
            return std::find(ids.begin(),ids.end(),entry.id)!=ids.end();}),updated.end());
        if(!Save(updated,error))return false;
        entries_.swap(updated);return true;
    } catch(const std::bad_alloc&) { error=BookmarkText("内存不足，无法移除书签", "Not enough memory to remove bookmarks");return false; }
}
} // namespace reader
