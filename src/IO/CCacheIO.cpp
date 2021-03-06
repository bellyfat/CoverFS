#include"Logger.h"
#include "CCacheIO.h"
#include <cassert>

// -----------------------------------------------------------------

CBlock::CBlock(CCacheIO &_cio, CEncrypt &_enc, int _blockidx, int size) : nextdirtyidx(-1), blockidx(_blockidx), cio(_cio), enc(_enc), buf(size), count(0)
{}

int8_t* CBlock::GetBufRead()
{
    mutex.lock();
    count++;
    if (cio.cryptcache)
        enc.Decrypt(blockidx, &buf[0]);
    return &buf[0];
}

int8_t* CBlock::GetBufReadWrite()
{
    int8_t* buf = GetBufRead();
    if (nextdirtyidx == -1)
    {
        nextdirtyidx = cio.lastdirtyidx.exchange(blockidx, std::memory_order_relaxed);
        cio.ndirty++;
    }
    return buf;
}

int8_t* CBlock::GetBufUnsafe()
{
    return &buf[0];
}


void CBlock::ReleaseBuf()
{
    if (cio.cryptcache)
        enc.Encrypt(blockidx, &buf[0]);
    mutex.unlock();
}

// -----------------------------------------------------------------

CCacheIO::CCacheIO(const std::shared_ptr<CAbstractBlockIO> &_bio, CEncrypt &_enc, bool _cryptcache) :
    bio(_bio), enc(_enc), ndirty(0), lastdirtyidx(-1), terminatesyncthread(false), cryptcache(_cryptcache)
{
    blocksize = bio->blocksize;
    syncthread = std::thread(&CCacheIO::Async_Sync, this);
}

CCacheIO::~CCacheIO()
{
    LOG(LogLevel::DEBUG) << "Cache: destruct";
    terminatesyncthread.store(true);
    Sync();
    syncthread.join();
    assert(ndirty.load() == 0);
    LOG(LogLevel::DEBUG) << "All Blocks stored. Erase cache ...";

    cachemtx.lock();
    for(auto iter = cache.begin(); iter != cache.end();)
    {
        CBLOCKPTR block = iter->second;
        if (block.use_count() != 2)
        {
            LOG(LogLevel::WARN) << "Block " << block->blockidx << " still in use.";
            iter++;
            continue;
        }
        if (!block->mutex.try_lock())
        {
            LOG(LogLevel::WARN) << "Locking block " << block->blockidx << " failed.";
            iter++;
            continue;
        }
        iter = cache.erase(iter);
        block->mutex.unlock();
    }
    LOG(LogLevel::DEBUG) << "Cache erased";

    if (!cache.empty())
    {
        LOG(LogLevel::WARN) << "Cache not empty";
    }
    cachemtx.unlock();
}

CBLOCKPTR CCacheIO::GetBlock(const int blockidx, bool read)
{
    cachemtx.lock();
    auto cacheblock = cache.find(blockidx);
    if (cacheblock != cache.end())
    {
        cachemtx.unlock();
        return cacheblock->second;
    }
    CBLOCKPTR block(new CBlock(*this, enc, blockidx, blocksize));
    cache[blockidx] = block;
    block->mutex.lock();
    cachemtx.unlock();
    if (read)
    {
        bio->Read(blockidx, 1, block->GetBufUnsafe());
        if (!cryptcache)
            enc.Decrypt(blockidx, block->GetBufUnsafe());
    }
    block->mutex.unlock();

    return block;
}

void CCacheIO::BlockReadForce(const int blockidx, const int n)
{
    if (n <= 0) return;
    auto *buf = new int8_t[blocksize*n];
    bio->Read(blockidx, n, buf);
    cachemtx.lock();
    for(int i=0; i<n; i++)
    {
        auto cacheblock = cache.find(blockidx+i);
        assert(cacheblock != cache.end()); // block created in CacheBlocks
        CBLOCKPTR block = cacheblock->second;
        memcpy(block->GetBufUnsafe(), &buf[i*blocksize], blocksize);
        if (!cryptcache)
            enc.Decrypt(blockidx+i, block->GetBufUnsafe());
        block->mutex.unlock();
    }
    cachemtx.unlock();
    delete[] buf;
}

void CCacheIO::CacheBlocks(const int blockidx, const int n)
{
    if (n <= 0) return;
    cachemtx.lock();
    int istart = 0;
    for(int i=0; i<n; i++)
    {
        auto cacheblock = cache.find(blockidx+i);
        if (cacheblock != cache.end())
        {
            int npart = i-istart;
            cachemtx.unlock();
            BlockReadForce(blockidx+istart, npart);
            cachemtx.lock();
            istart = i+1;
        } else
        {
            CBLOCKPTR block(new CBlock(*this, enc, blockidx+i, blocksize));
            cache[blockidx+i] = block;
            block->mutex.lock();
        }
    }
    int npart = n-istart;
    cachemtx.unlock();
    BlockReadForce(blockidx+istart, npart);
}

int64_t CCacheIO::GetFilesize()
{
    return bio->GetFilesize();
}

int64_t CCacheIO::GetNDirty()
{
    return ndirty.load();
}

int64_t CCacheIO::GetNCachedBlocks()
{
    cachemtx.lock();
    int64_t n = cache.size();
    cachemtx.unlock();
    return n;
}


void CCacheIO::Async_Sync()
{
    int8_t buf[blocksize];
    for(;;)
    {
        while (ndirty.load() == 0)
        {
            if (terminatesyncthread.load()) return;
            std::unique_lock<std::mutex> lock(async_sync_mutex);
            async_sync_cond.wait(lock);
        }

        int nextblockidx = lastdirtyidx.exchange(-1, std::memory_order_relaxed);
        while(nextblockidx != -1)
        {
            cachemtx.lock();
            CBLOCKPTR block = cache.find(nextblockidx)->second;
            block->mutex.lock(); // TODO trylock and put back on the list
            cachemtx.unlock();
            nextblockidx = block->nextdirtyidx;
            memcpy(buf, block->GetBufUnsafe(), blocksize);
            block->nextdirtyidx = -1;
            ndirty--;
            block->mutex.unlock();

            if (!cryptcache)
                enc.Encrypt(block->blockidx, buf);
            bio->Write(block->blockidx, 1, buf);
        }
    }
}


void CCacheIO::Sync()
{
    async_sync_cond.notify_one();
}

// -----------------------------------------------------------------

void CCacheIO::Read(int64_t ofs, int64_t size, int8_t *d)
{
    CBLOCKPTR block;
    int8_t *buf = nullptr;
    //printf("ReadFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    int firstblock = ofs/blocksize;
    int lastblock = (ofs+size-1)/blocksize;

    CacheBlocks(firstblock, lastblock-firstblock+1);

    int64_t dofs = 0;
    for(int64_t j=firstblock; j<=lastblock; j++)
    {
        //printf("GetBlock %li\n", j);
        block = GetBlock(j, true);
        //printf("GetBuf %li\n", j);
        buf = block->GetBufRead();
        int bsize = blocksize - (ofs%blocksize);
        bsize = std::min((int64_t)bsize, size);
        memcpy(&d[dofs], &buf[ofs%blocksize], bsize);
        ofs += bsize;
        dofs += bsize;
        size -= bsize;
        block->ReleaseBuf();
    }
}

void CCacheIO::Write(int64_t ofs, int64_t size, const int8_t *d)
{
    CBLOCKPTR block;
    int8_t *buf = NULL;
    //printf("WriteFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    int firstblock = ofs/blocksize;
    int lastblock = (ofs+size-1)/blocksize;

    // check which blocks we have to read
    if ((ofs%blocksize) != 0) block = GetBlock(firstblock, true);
    if (((ofs+size-1)%blocksize) != 0) block = GetBlock(lastblock, true);

    int64_t dofs = 0;
    for(int64_t j=firstblock; j<=lastblock; j++)
    {
        block = GetBlock(j, false);
        buf = block->GetBufReadWrite();
        int bsize = blocksize - (ofs%blocksize);
        bsize = std::min((int64_t)bsize, size);
        memcpy(&buf[ofs%blocksize], &d[dofs], bsize);
        ofs += bsize;
        dofs += bsize;
        size -= bsize;
        block->ReleaseBuf();
    }
    Sync();
}

void CCacheIO::Zero(int64_t ofs, int64_t size)
{
    CBLOCKPTR block;
    int8_t *buf = nullptr;
    //printf("ZeroFragment ofs=%li size=%li\n", ofs, size);
    if (size == 0) return;

    int firstblock = ofs/blocksize;
    int lastblock = (ofs+size-1)/blocksize;

    // check which blocks we have to read
    if ((ofs%blocksize) != 0)          block = GetBlock(firstblock, true);
    if (((ofs+size-1)%blocksize) != 0) block = GetBlock(lastblock, true);

    int64_t dofs = 0;
    for(int64_t j=firstblock; j<=lastblock; j++)
    {
        block = GetBlock(j, false);
        buf = block->GetBufReadWrite();
        int bsize = blocksize - (ofs%blocksize);
        bsize = std::min((int64_t)bsize, size);
        memset(&buf[ofs%blocksize], 0, bsize);
        ofs += bsize;
        dofs += bsize;
        size -= bsize;
        block->ReleaseBuf();
    }
    Sync();
}
