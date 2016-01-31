#include "CSimpleFS.h"
#include "CDirectory.h"

#include<stdio.h>
#include<assert.h>
#include<set>
#include<algorithm>
#include<climits>

/*
TODO:
    - readahead
    - faster read
    - remove should check for shared_ptr number of pointers
    - gcrypt ctr mode correctly used?
    - ReleaseBuf on destroy???
*/

// -------------------------------------------------------------

SimpleFilesystem::SimpleFilesystem(CCacheIO &_bio) : bio(_bio), nodeinvalid(new INODE(*this))
{
    nodeinvalid->id = INVALIDID;

    assert(sizeof(DIRENTRY) == 128);
    assert(sizeof(size_t) == 8);
    assert(sizeof(CFragmentDesc) == 16);

    printf("container info:\n");
    printf("  size: %i MB\n", int(bio.GetFilesize()/(1024*1024)));
    printf("  blocksize: %i\n", bio.blocksize);

    CBLOCKPTR superblock = bio.GetBlock(1);
    int8_t* buf = superblock->GetBuf();
    if (strncmp((char*)buf, "CoverFS", 7) == 0)
    {
        printf("superblock %s valid\n", buf);
        superblock->ReleaseBuf();

        unsigned int nfragmentblocks = 5;
        unsigned int nentries = bio.blocksize*nfragmentblocks/16;
        printf("  number of blocks containing fragments: %i with %i entries\n", nfragmentblocks, nentries);

        fragmentblocks.clear();
        for(unsigned int i=0; i<nfragmentblocks; i++)
            fragmentblocks.push_back(bio.GetBlock(i+2));

        ofssort.assign(nentries, 0);
        idssort.assign(nentries, 0);
        for(unsigned int i=0; i<nentries; i++) {ofssort[i] = i; idssort[i] = i;}

        fragments.assign(nentries, CFragmentDesc(FREEID, 0, 0));

        for(unsigned int i=0; i<nfragmentblocks; i++)
        {
            int nidsperblock = bio.blocksize / 16;
            CBLOCKPTR block = fragmentblocks[i];
            int8_t* buf = block->GetBuf();
            memcpy(&fragments[i*nidsperblock], buf, sizeof(CFragmentDesc)*nidsperblock);
            block->ReleaseBuf();
        
        }
        SortOffsets();
        printf("\n");
        return;
    }
    superblock->ReleaseBuf();

    CreateFS();	
}


void SimpleFilesystem::CreateFS()
{
    printf("==================\n");
    printf("Create Filesystem\n");

    printf("  Write superblock\n");

    CBLOCKPTR superblock = bio.GetBlock(1);
    int8_t* buf = superblock->GetBuf();
    strncpy((char*)buf, "CoverFS", 8);
    superblock->ReleaseBuf();
    superblock->Dirty();
    bio.Sync();

    unsigned int nfragmentblocks = 5;
    unsigned int nentries = bio.blocksize*nfragmentblocks/16;
    printf("  number of blocks containing fragment: %i with %i entries\n", nfragmentblocks, nentries);
// ---
    fragmentblocks.clear();
    for(unsigned int i=0; i<nfragmentblocks; i++)
    {
        fragmentblocks.push_back(bio.GetBlock(i+2));
    }
// ---

    ofssort.assign(nentries, 0);
    for(unsigned int i=0; i<nentries; i++)
    {
        ofssort[i] = i;
    }

    fragments.assign(nentries, CFragmentDesc(FREEID, 0, 0));
    fragments[0] = CFragmentDesc(SUPERID, 0, bio.blocksize*2);
    fragments[1] = CFragmentDesc(TABLEID, 2, bio.blocksize*nfragmentblocks);

    SortOffsets();

// ---

    for(unsigned int i=0; i<nentries; i++)
        StoreFragment(i);

    bio.Sync();
// ---

    // Create root directory

    nodeinvalid->id = INVALIDID;
    nodeinvalid->type = INODETYPE::dir;
    CDirectory rootdir = CDirectory(nodeinvalid, *this);
    int id = rootdir.CreateDirectory(std::string("root"));
    nodeinvalid->id = INVALIDID;
    nodeinvalid->type = INODETYPE::unknown;

    if (id != ROOTID)
    {
        fprintf(stderr, "Error: Cannot create root directory\n");
        exit(1);
    }

    CDirectory dir = OpenDir("/");
    dir.CreateDirectory("mydir");        

    dir.CreateFile("hello");
    INODEPTR node = OpenNode("hello");
    const char *s = "Hello world\n";
    node->Write((int8_t*)s, 0, strlen(s));
    printf("Filesystem created\n");
    printf("==================\n");
}

void SimpleFilesystem::StoreFragment(int idx)
{
    int nidsperblock = bio.blocksize / 16;
    CBLOCKPTR block = fragmentblocks[idx/nidsperblock];
    int8_t* buf = block->GetBuf();
    ((CFragmentDesc*)buf)[idx%nidsperblock] = fragments[idx];
    block->ReleaseBuf();
    block->Dirty();
}

INODEPTR SimpleFilesystem::OpenNode(int id)
{
    auto it = inodes.find(id);
    if (it != inodes.end())
    {
        //it->second->Print();
        //printf("Open File with id=%i blocks=%zu and ptrcount=%li\n", id, it->second->blocks.size(), it->second.use_count());
        assert(id == it->second->id);
        return it->second;
    }

    INODEPTR node(new INODE(*this));
    node->id = id;
    node->size = 0;
    node->fragments.clear();
    node->parentid = INVALIDID;

    if (id == ROOTID)
    {
        node->type = INODETYPE::dir;
    }

    for(unsigned int i=0; i<fragments.size(); i++)
    {
        if (fragments[i].id != id) continue;
        //printf("OpenNode id=%i: Add fragment with index %i with starting_block=%i size=%i bytes\n", node->id, i, fragments[i].ofs, fragments[i].size);
        node->size += fragments[i].size;
        node->fragments.push_back(i);
    }
    assert(node->fragments.size() > 0);
    inodes[id] = node;
    //printf("Open File with id=%i blocks=%zu\n", id, node->blocks.size());
    return node;
}

std::vector<std::string> SplitPath(const std::string &path)
{
    std::vector<std::string> d;
    std::string s = "";

    unsigned int idx = 0;

    while(idx<path.size())
    {
        if ((path[idx] == '/') || (path[idx] == '\\'))
        {
            if (s.size() != 0)
            {
                d.push_back(s);
                s = "";
            }
            idx++;
            continue;
        }
        s = s + path[idx];
        idx++;
    }
    if (s.size() != 0) d.push_back(s);
    /*
        for(unsigned int i=0; i<d.size(); i++)
                printf("  %i: %s\n", i, d[i].c_str());
*/
    return d;
}

INODEPTR SimpleFilesystem::OpenNode(const std::string &path)
{
    assert(path.size() != 0);
    std::vector<std::string> splitpath;
    splitpath = SplitPath(path);
    return OpenNode(splitpath);
}

INODEPTR SimpleFilesystem::OpenNode(const std::vector<std::string> splitpath)
{
    INODEPTR node;
    DIRENTRY e("");

    int dirid = 0;
    e.id = 0;
    e.type = (int32_t)INODETYPE::dir;
    for(unsigned int i=0; i<splitpath.size(); i++)
    {
        dirid = e.id;
        node = OpenNode(dirid);
        CDirectory(node, *this).Find(splitpath[i], e);
        if (e.id == INVALIDID) 
        {
            throw ENOENT;
        }
        if (i<splitpath.size()-1) assert((INODETYPE)e.type == INODETYPE::dir);
    }

    node = OpenNode(e.id);
    node->parentid = dirid;
    node->type = (INODETYPE)e.type; // static cast?
    if (splitpath.empty()) 
        node->name = "/";
    else 
        node->name = splitpath.back();

    return node;
}

CDirectory SimpleFilesystem::OpenDir(int id)
{
    INODEPTR node = OpenNode(id);
    node->type = INODETYPE::dir; // if opened this way, we have to assume, that this is indeed a dir.
    return CDirectory(node, *this);
}

CDirectory SimpleFilesystem::OpenDir(const std::string &path)
{
    INODEPTR node = OpenNode(path);
    return CDirectory(node, *this);
}

CDirectory SimpleFilesystem::OpenDir(const std::vector<std::string> splitpath)
{
    INODEPTR node = OpenNode(splitpath);
    return CDirectory(node, *this);
}

INODEPTR SimpleFilesystem::OpenFile(const std::string &path)
{
    INODEPTR node = OpenNode(path);
    if (node->id == INVALIDID) throw ENOENT;
    if (node->type != INODETYPE::file) throw ENOENT;
    return node;
}

INODEPTR SimpleFilesystem::OpenFile(const std::vector<std::string> splitpath)
{
    INODEPTR node = OpenNode(splitpath);
    if (node->id == INVALIDID) throw ENOENT;
    if (node->type != INODETYPE::file) throw ENOENT;
    return node;
}

void SimpleFilesystem::CheckFS()
{
    // check for overlap
    SortOffsets();

    printf("Check for overlap\n");
    int idx1, idx2;
    for(unsigned int i=0; i<ofssort.size(); i++)
    {
        idx1 = ofssort[i+0];
        idx2 = ofssort[i+1];

        int nextofs = fragments[idx1].GetNextFreeOfs(bio.blocksize); 
        if (fragments[idx2].size == 0) break;
        if (fragments[idx2].id == FREEID) break;
        int64_t hole = (fragments[idx2].ofs  - nextofs)*bio.blocksize;
        if (hole < 0)
        {
            fprintf(stderr, "Error in CheckFS: fragment overlap detected");
            exit(1);
        }
    }
}

CExtFragmentDesc SimpleFilesystem::GetNextFreeFragment(INODE &node, int64_t maxsize)
{
    CExtFragmentDesc ebe(-1, CFragmentDesc(FREEID, 0, 0));
    assert(node.fragments.size() != 0);

    int lastidx = node.fragments.back();

    std::lock_guard<std::mutex> lock(inodetablemtx);

    // first find free id
    ebe.storeidx = -1;
    for(unsigned int i=lastidx+1; i<fragments.size(); i++)
    {
        if (fragments[i].id != FREEID) continue;
        ebe.storeidx = i;
        break;
    }
    assert(ebe.storeidx != -1); // TODO: change list size in this case

    ebe.be.id = node.id;

    //printf("  found next free fragment: storeidx=%i\n", ebe.storeidx);

    // now search for a big hole
    int idx1, idx2;
    for(unsigned int i=0; i<ofssort.size(); i++)
    {
        idx1 = ofssort[i+0];
        idx2 = ofssort[i+1];

        //printf("  analyze fragment %i with ofsblock=%li size=%u of id=%i\n", idx1, fragments[idx1].ofs, fragments[idx1].size, fragments[idx1].id);
        int nextofs = fragments[idx1].GetNextFreeOfs(bio.blocksize); 
        if (fragments[idx2].size == 0) break;
        if (fragments[idx2].id == FREEID) break;

        int64_t hole = (fragments[idx2].ofs  - nextofs)*bio.blocksize;
        assert(hole >= 0);

        // prevent fragmentation
        if ((hole > 0x100000) || (hole > maxsize/4))
        {
            ebe.be.size = hole;
            ebe.be.ofs = nextofs;
            return ebe;
        }
    }

    // No hole found, so put it at the end
    //printf("no hole found\n");
    ebe.be.size = 0xFFFFFFFF;
    if (fragments[idx1].size == 0)
        ebe.be.ofs = fragments[idx1].ofs;
    else
        ebe.be.ofs = fragments[idx1].ofs + (fragments[idx1].size-1)/bio.blocksize + 1;
    //printf("new ofsblock %li\n", ebe.be.ofs);
    return ebe;
}

void SimpleFilesystem::SortOffsets()
{
    std::sort(ofssort.begin(),ofssort.end(), [&](int a, int b)
    {
        int ofs1 = fragments[a].ofs;
        int ofs2 = fragments[b].ofs;
        if (fragments[a].size == 0) ofs1 = INT_MAX;
        if (fragments[b].size == 0) ofs2 = INT_MAX;
        if (fragments[a].id == FREEID) ofs1 = INT_MAX;
        if (fragments[b].id == FREEID) ofs2 = INT_MAX;
        return ofs1 < ofs2;
    });
}

void SimpleFilesystem::SortIDs()
{
    std::sort(idssort.begin(),idssort.end(), [&](int a, int b)
    {
        int id1 = fragments[a].id;
        int id2 = fragments[b].id;
        if (fragments[a].id == FREEID) id1 = INT_MAX;
        if (fragments[b].id == FREEID) id2 = INT_MAX;
        return id1 < id2;
    });
}

void SimpleFilesystem::Truncate(INODE &node, int64_t size, bool dozero)
{
    //printf("Truncate of id=%i to:%li from:%li\n", node.id, size, node.size);

    assert(node.id != INVALIDID);
    std::lock_guard<std::recursive_mutex> lock(node.GetMutex());
    if (size == node.size) return;

    if (size > node.size)
    {
        while(node.size < size)
        {
            CExtFragmentDesc ebe = GetNextFreeFragment(node, size-node.size);
            ebe.be.id = node.id;
            ebe.be.size = std::min( size-node.size, (int64_t)ebe.be.size);
            if (dozero)
                ZeroFragment(ebe.be.ofs*bio.blocksize, ebe.be.size);
            uint64_t nextofs = fragments[node.fragments.back()].GetNextFreeOfs(bio.blocksize);

            if (fragments[node.fragments.back()].size == 0) // empty fragmentn can be overwritten
            {
                ebe.storeidx = node.fragments.back();
                fragments[ebe.storeidx] = ebe.be;
            } else
            if (nextofs == ebe.be.ofs) // merge
            { // merge
                ebe.storeidx = node.fragments.back();

                if (dozero)
                ZeroFragment(
                    fragments[ebe.storeidx].ofs*bio.blocksize+fragments[ebe.storeidx].size, 
                    nextofs*bio.blocksize - (fragments[ebe.storeidx].ofs*bio.blocksize + fragments[ebe.storeidx].size)
                );
                if (node.size+(int64_t)ebe.be.size > 0xFFFFFFFF)
                {
                    exit(1);
                    // TODO: check for 4GB bouondary
                }
                fragments[ebe.storeidx].size += ebe.be.size;
            } else
            {
                fragments[ebe.storeidx] = ebe.be;
                node.fragments.push_back(ebe.storeidx);
            }
            node.size += ebe.be.size;
            StoreFragment(ebe.storeidx);
            SortOffsets();
        }
    } else
    if (size < node.size)
    {
        while(node.size > 0)
        {
            int lastidx = node.fragments.back();
            CFragmentDesc &r = fragments[lastidx];
            node.size -= r.size;
            r.size = std::max(size-node.size, 0L);
            node.size += r.size;

            if ((r.size == 0) && (node.size != 0)) // don't remove last element
            {
                r.id = FREEID;
                StoreFragment(lastidx);
                node.fragments.pop_back();
            } else
            {
                StoreFragment(lastidx);
                break;
            }        
        }
        SortOffsets();
    }
    bio.Sync();
}

// ------------

class CFragmentOverlap
{
    public:
    CFragmentOverlap(int64_t _ofs=0, int64_t _size=0) : ofs(_ofs), size(_size) {}
    int64_t ofs;
    int64_t size;
};

bool FindIntersect(const CFragmentOverlap &a, const CFragmentOverlap &b, CFragmentOverlap &i)
{
    if (a.ofs > b.ofs) i.ofs = a.ofs; else i.ofs = b.ofs;
    if (a.ofs+a.size > b.ofs+b.size) i.size = b.ofs+b.size-i.ofs; else i.size = a.ofs+a.size-i.ofs;
    if (i.size <= 0) return false;
    return true;
}

// -----------

void SimpleFilesystem::ZeroFragment(int64_t ofs, int64_t size)
{
    int currentblockidx = -1;
    CBLOCKPTR block;
    int8_t *buf = NULL;
    //printf("ZeroFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    bio.CacheBlocks(ofs/bio.blocksize, size/bio.blocksize);

    for(int64_t j=0; j<size; j++)
    {
        int blockidx = ofs / bio.blocksize;
        if (blockidx != currentblockidx)
        {
            if (currentblockidx != -1) 
            {
                block->ReleaseBuf();
                block->Dirty();
            }
            currentblockidx = blockidx;                        
            block = bio.GetBlock(currentblockidx);
            buf = block->GetBuf();
        }
        buf[ofs & (bio.blocksize-1)] = 0x0;
        ofs++;
    }
    assert(currentblockidx != -1);
    block->ReleaseBuf();
    block->Dirty();    
    bio.Sync();
}


// Copy d to ofs in container
void SimpleFilesystem::WriteFragment(int64_t ofs, const int8_t *d, int64_t size)
{
    int currentblockidx = -1;
    CBLOCKPTR block;
    int8_t *buf = NULL;
    //printf("WriteFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    bio.CacheBlocks(ofs/bio.blocksize, size/bio.blocksize);

    for(int64_t j=0; j<size; j++)
    {
        int blockidx = ofs / bio.blocksize;
        if (blockidx != currentblockidx)
        {
            if (currentblockidx != -1)
            {
                block->ReleaseBuf(); // we should release it, when we finished reading, but here it doesn't matter
                block->Dirty();
            }
            currentblockidx = blockidx;
            block = bio.GetBlock(currentblockidx);
            buf = block->GetBuf();
        }
        buf[ofs & (bio.blocksize-1)] = d[j];
        ofs++;
    }
    assert(currentblockidx != -1);
    block->ReleaseBuf();
    block->Dirty();
    bio.Sync();
}

void SimpleFilesystem::ReadFragment(int64_t ofs, int8_t *d, int64_t size)
{
    int currentblockidx = -1;
    CBLOCKPTR block;
    int8_t *buf = NULL;
    //printf("ReadFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    bio.CacheBlocks(ofs/bio.blocksize, size/bio.blocksize);

    for(int64_t j=0; j<size; j++)
    {
        int blockidx = ofs / bio.blocksize;
        if (blockidx != currentblockidx)
        {
            if (currentblockidx != -1) 
            {
                block->ReleaseBuf();
            }
            currentblockidx = blockidx;                        
            block = bio.GetBlock(currentblockidx);
            buf = block->GetBuf();
        }
        d[j] = buf[ofs & (bio.blocksize-1)];
        ofs++;
    }
    assert(currentblockidx != -1);
    block->ReleaseBuf();
}

// -----------

int64_t SimpleFilesystem::Read(INODE &node, int8_t *d, int64_t ofs, int64_t size)
{
    int64_t s = 0;
    //printf("read node.id=%i node.size=%li read_ofs=%li read_size=%li\n", node.id, node.size, ofs, size);

    if (size == 0) return size;

    std::lock_guard<std::recursive_mutex> lock(node.GetMutex());
    int64_t fragmentofs = 0x0;
    for(unsigned int i=0; i<node.fragments.size(); i++)
    {
        int idx = node.fragments[i];
        assert(fragments[idx].id == node.id);
        CFragmentOverlap intersect;
        if (FindIntersect(CFragmentOverlap(fragmentofs, fragments[idx].size), CFragmentOverlap(ofs, size), intersect))
        {
            assert(intersect.ofs >= ofs);
            assert(intersect.ofs >= fragmentofs);
            ReadFragment(
                fragments[idx].ofs*bio.blocksize + (intersect.ofs - fragmentofs), 
                &d[intersect.ofs-ofs], 
                intersect.size);
            s += intersect.size;
        }
        fragmentofs += fragments[idx].size;
    }
    //bio.Sync();
    return s;
}

void SimpleFilesystem::Write(INODE &node, const int8_t *d, int64_t ofs, int64_t size)
{
    if (size == 0) return;

    //printf("write node.id=%i node.size=%li write_ofs=%li write_size=%li\n", node.id, node.size, ofs, size);
    std::lock_guard<std::recursive_mutex> lock(node.GetMutex());

    if (node.size < ofs+size) Truncate(node, ofs+size, false);
    
    int64_t fragmentofs = 0x0;
    for(unsigned int i=0; i<node.fragments.size(); i++)
    {
        int idx = node.fragments[i];
	CFragmentOverlap intersect;
        if (FindIntersect(CFragmentOverlap(fragmentofs, fragments[idx].size), CFragmentOverlap(ofs, size), intersect))
        {
            assert(intersect.ofs >= ofs);
            assert(intersect.ofs >= fragmentofs);
            WriteFragment(
                fragments[idx].ofs*bio.blocksize + (intersect.ofs - fragmentofs), 
                &d[intersect.ofs-ofs], 
                intersect.size);
        }
        fragmentofs += fragments[idx].size;
    }
    bio.Sync();
}

void SimpleFilesystem::PrintFS()
{
    SortOffsets();

    printf("Fragment List:\n");
    for(unsigned int i=0; i<ofssort.size(); i++)
    {
        //int idx1 = ofssort[i];
        int idx1 = i;
        if (fragments[idx1].id == FREEID) continue;
        printf("  fragment %4i id=%4i with ofsblock=%li size=%u\n", idx1, fragments[idx1].id, fragments[idx1].ofs, fragments[idx1].size);
    }

    std::set<int32_t> s;
    int64_t size=0;
    for(unsigned int i=0; i<fragments.size(); i++)
    {
        int32_t id = fragments[i].id;
        if (id >= 0) 
	{
		size += fragments[i].size;
		s.insert(id);
	}
    }
    printf("  number of inodes: %li\n", s.size());
    printf("  stored bytes: %li\n", size);
    printf("  container usage: %f %%\n", (double)size/(double)bio.GetFilesize()*100.);

    // very very slow
    SortIDs();
    //int frags[5] = {0};
    for(auto f : s) 
    {
        int nfragments = 0;
        for(unsigned int i=0; i<fragments.size(); i++)
        {
            if (fragments[i].id == f) nfragments++;
        }
        printf("id=%4i fragments=%4i\n", f, nfragments);
/*
        if (fragments > 4) nfragments = 4;
        assert(nfragnents != 0);
        frags[nfragments]++;
*/
    }
}

// -----------

void SimpleFilesystem::Rename(INODEPTR &node, CDirectory &newdir, const std::string &filename)
{
    DIRENTRY e(filename);
    CDirectory olddir = OpenDir(node->parentid);
    std::lock_guard<std::recursive_mutex> lock(olddir.dirnode->GetMutex());
    olddir.Find(node->name, e);
    olddir.RemoveEntry(node->name);
    strncpy(e.name, filename.c_str(), 64+32);
    /*        
    // check if file already exist and remove it        
    // this is already done in fuserop
    newdir.Find(filename, e);
    DIRENTRY e(filename);
    if (e.id != INVALIDID)
    {
    }
    */
    newdir.AddEntry(e);
}


int SimpleFilesystem::ReserveNewFragment()
{
    std::lock_guard<std::mutex> lock(inodetablemtx);

    int idmax = -1;
    for(unsigned int i=0; i<fragments.size(); i++)
    {
        if (fragments[i].id > idmax) idmax = fragments[i].id;
    }
    int id = idmax+1;
    //printf("get free id %i\n", id);

    for(unsigned int i=0; i<fragments.size(); i++)
    {
        if (fragments[i].id != FREEID) continue;
        fragments[i] = CFragmentDesc(id, 0, 0);
        StoreFragment(i);
        SortOffsets();
        return id;
    }
    fprintf(stderr, "Error: No free blocks available\n");
    exit(1);
    return id;
}

int SimpleFilesystem::CreateNode(CDirectory &dir, const std::string &name, INODETYPE t)
{
    // Reserve one block. Necessary even for empty files
    int id = ReserveNewFragment();
    bio.Sync();
    if (dir.dirnode->id == INVALIDID) return id; // this is the root directory and does not have a parent
    dir.AddEntry(DIRENTRY(name, id, t));
    return id;
}

int SimpleFilesystem::CreateFile(CDirectory &dir, const std::string &name)
{
    int id = CreateNode(dir, name, INODETYPE::file);
    //printf("Create File '%s' with id=%i\n", name.c_str(), id);
    return id;
}

int SimpleFilesystem::CreateDirectory(CDirectory &dir, const std::string &name)
{
    int id = CreateNode(dir, name, INODETYPE::dir);
    //printf("Create Directory '%s' with id=%i\n", name.c_str(), id);

    INODEPTR newdirnode = OpenNode(id);
    newdirnode->parentid = dir.dirnode->id;
    newdirnode->name = name;
    newdirnode->type = INODETYPE::dir;

    CDirectory(newdirnode, *this).Create();
    return id;
}

void SimpleFilesystem::FreeAllFragments(INODE &node)
{
    for(unsigned int i=0; i<node.fragments.size(); i++)
    {
        fragments[node.fragments[i]].id = FREEID;
        StoreFragment(node.fragments[i]);
    }
    SortOffsets();
    node.fragments.clear();
    bio.Sync();
}

void SimpleFilesystem::Remove(INODE &node)
{
    //maybe, we have to check the shared_ptr here
    CDirectory dir = OpenDir(node.parentid);
    std::lock_guard<std::recursive_mutex> lock(dir.dirnode->GetMutex());
    FreeAllFragments(node);
    dir.RemoveEntry(node.name);
    inodes.erase(node.id); // remove from map
}

void SimpleFilesystem::StatFS(struct statvfs *buf)
{
    buf->f_bsize   = bio.blocksize;
    buf->f_frsize  = bio.blocksize;
    buf->f_blocks  = bio.GetFilesize()/bio.blocksize;
    buf->f_namemax = 64+31;
    buf->f_bfree   = 0;

    std::set<int32_t> s;
    for(unsigned int i=0; i<fragments.size(); i++)
    {
        int32_t id = fragments[i].id;
        if (id == FREEID) buf->f_bfree += (fragments[i].size-1)/bio.blocksize+1;
        if (id >= 0) s.insert(id);
    }
    buf->f_bavail  = buf->f_bfree;
    buf->f_files   = s.size();
}
