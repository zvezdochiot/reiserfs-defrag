#include "reiserfs.hpp"
#include <string.h>
#include <iostream>
#include <algorithm>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <assert.h>

FsJournal::FsJournal(int fd_)
{
    this->fd = fd_;
    this->cache_hits = 0;
    this->cache_misses = 0;
    this->max_cache_size = 51200;
    this->transaction.running = false;
}

FsJournal::~FsJournal()
{
    // purge cache will be performed automatically
    std::map<uint32_t, cache_entry>::iterator it, dit;
    it = this->block_cache.begin();
    while (it != this->block_cache.end()) {
        dit = it ++;
        this->deleteFromCache(dit->first);
    }
    if (this->block_cache.size() > 0) {
        std::cerr << "error: FsJournal::block_cache is not empty" << std::endl;
        assert (false);
    }
    std::cout << "cache hits = " << this->cache_hits <<
        ", cache_misses = " << this->cache_misses << std::endl;
}

void
FsJournal::beginTransaction()
{
    if (this->transaction.running) {
        std::cerr << "error: nested transaction" << std::endl;
        return; // TODO: error handling
    }
    if (this->transaction.blocks.size() != 0) {
        std::cerr << "error: there was writes outside transaction" << std::endl;
        return; // TODO: error handling
    }

    this->transaction.running = true;
}

template<typename Tn>
void sortAndRemoveDuplicates(std::vector<Tn> &v)
{
    // first sort
    std::sort (v.begin(), v.end());
    // then deduplicate and erase trailing garbage
    v.erase (std::unique (v.begin(), v.end()), v.end());
}

void
FsJournal::commitTransaction()
{

    if (this->transaction.blocks.size() == 0) {
        // std::cout << "warning: empty transaction" << std::endl;
        this->transaction.running = false;
        return;
    }

    // remove duplicate entries
    sortAndRemoveDuplicates(this->transaction.blocks);

    std::cout << "Journal: transaction size = " << this->transaction.blocks.size() << std::endl;

    // check if any of blocks are dirty. They all must be not.
    for (std::vector<Block *>::const_iterator it = this->transaction.blocks.begin();
        it != this->transaction.blocks.end(); ++ it)
    {
        std::cout << (*it)->block << std::endl;
        assert ((*it)->dirty == false); // must not be dirty
    }

    // TODO: * allocate memory enough to make journal entry
    // TODO: * construct journal entry
    // TODO: * write journal entry
    // TODO: * update journal header
    // TODO: * update data on disk
    // TODO: * close trasaction

    // finally release blocks. Block can survive this if it has more than reference.
    // So do cached blocks. ->releaseBlock will not call writeBlock as block is not
    // dirty.
    for (std::vector<Block *>::const_iterator it = this->transaction.blocks.begin();
        it != this->transaction.blocks.end(); ++ it)
    {
        this->releaseBlock(*it);
    }

    this->transaction.blocks.resize(0);
    this->transaction.running = false;
    ::fsync(this->fd);
}

Block*
FsJournal::readBlock(uint32_t block_idx, bool caching)
{
    // check if cache have this block
    if (this->blockInCache(block_idx)) {
        if (caching) this->cache_hits ++;
        this->touchCacheEntry(block_idx);
        this->block_cache[block_idx].block_obj->ref_count ++;
        return this->block_cache[block_idx].block_obj;
    }
    this->cache_misses ++;
    // not found, read from disk
    off_t new_ofs = ::lseek (this->fd, static_cast<off_t>(block_idx) * BLOCKSIZE, SEEK_SET);
    if (static_cast<off_t>(-1) == new_ofs) {
        std::cerr << "error: seeking" << std::endl;
        // TODO: error handling
        return NULL;
    }
    Block *block_obj = new Block(this);
    ssize_t bytes_read = ::read (this->fd, block_obj->buf, BLOCKSIZE);
    if (BLOCKSIZE != bytes_read) {
        std::cerr << "error: readBlock(" << block_idx << ")" << std::endl;
        return NULL;
    }
    block_obj->block = block_idx;
    if (caching) this->pushToCache(block_obj);
    return block_obj;
}

void
FsJournal::readBlock(Block &block_obj, uint32_t block_idx)
{
    off_t new_ofs = ::lseek (this->fd, (off_t)block_idx * BLOCKSIZE, SEEK_SET);
    if (static_cast<off_t>(-1) == new_ofs) {
        std::cerr << "error: seeking" << std::endl;
        // TODO: error handling
        return;
    }
    ssize_t bytes_read = ::read (this->fd, block_obj.buf, BLOCKSIZE);
    if (BLOCKSIZE != bytes_read) {
        std::cerr << "error: readBlock(" << &block_obj << ", " << block_idx << ")" << std::endl;
        return;
    }
    block_obj.block = block_idx;
}

void
FsJournal::writeBlock(Block *block_obj)
{
    off_t new_ofs = ::lseek (this->fd, static_cast<off_t>(block_obj->block) * BLOCKSIZE, SEEK_SET);
    if (static_cast<off_t>(-1) == new_ofs) {
        std::cerr << "error: seeking" << std::endl;
        // TODO: error handling
        return;
    }
    ssize_t bytes_written = ::write (this->fd, block_obj->buf, BLOCKSIZE);
    if (BLOCKSIZE != bytes_written) {
        std::cerr << "error: writeBlock(" << &block_obj << ")" << std::endl;
        return;
    }

    this->transaction.blocks.push_back(block_obj);
    block_obj->ref_count ++;
}

void
FsJournal::moveRawBlock(uint32_t from, uint32_t to, bool include_in_transaction)
{
    Block *block_obj = this->readBlock(from, false);
    this->deleteFromCache(block_obj->block);
    assert (block_obj->ref_count == 1);
    block_obj->markDirty();
    block_obj->block = to;

    if (include_in_transaction) {
        this->transaction.blocks.push_back(block_obj);
        block_obj->ref_count ++;
    }
    this->releaseBlock(block_obj);
}

void
FsJournal::releaseBlock(Block *block_obj)
{
    if (block_obj->dirty) {
        this->writeBlock(block_obj);
        block_obj->dirty = false;
    }
    block_obj->ref_count --;
    assert (block_obj->ref_count >= 0);
    if (block_obj->ref_count == 0)
        delete block_obj;
}

void
FsJournal::pushToCache(Block *block_obj)
{
    while (this->block_cache.size() >= this->max_cache_size - 1)
        this->eraseOldestCacheEntry();

    FsJournal::cache_entry ci;
    ci.block_obj = block_obj;
    this->block_cache[block_obj->block] = ci;
    block_obj->ref_count ++;
}

void
FsJournal::touchCacheEntry(uint32_t block_idx)
{

}

void
FsJournal::eraseOldestCacheEntry()
{
    // use Random Replacement
    std::map<uint32_t, cache_entry>::iterator it, dit;
    it = this->block_cache.begin();
    while (it != this->block_cache.end()) {
        dit = it ++;
        if (std::rand()%256 == 0) {
            this->deleteFromCache(dit->first);
        }
    }
}

void
FsJournal::deleteFromCache(uint32_t block_idx)
{
    if (this->block_cache.count(block_idx) > 0) {
        Block *block_obj = this->block_cache[block_idx].block_obj;
        this->releaseBlock(block_obj);
        this->block_cache.erase(block_idx);
    }
}
