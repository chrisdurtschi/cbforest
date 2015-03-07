//
//  RevTree.cc
//  CBForest
//
//  Created by Jens Alfke on 5/13/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "RevTree.hh"
#include "varint.hh"
#include "Delta.hh"

#include <forestdb.h>
#include <arpa/inet.h>  // for htons, etc.
#include <assert.h>
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ostream>
#include <sstream>

#define offsetby(PTR,OFFSET) (void*)((uint8_t*)(PTR)+(OFFSET))


namespace forestdb {

    // Since forestdb already applies a CRC checksum to the document, and both the
    // source and target of a delta come from the same doc, we can safely skip the
    // zdelta checksum.
    static const DeltaFlags kRevDeltaFlags = kNoChecksum;

    // Layout of revision rev in encoded form. Tree is a sequence of these followed by a 32-bit zero.
    // Revs are stored in decending priority, with the current leaf rev(s) coming first.
    struct RawRevision {
        // Private RevisionFlags bits used in encoded form:
        enum : uint8_t {
            kPublicPersistentFlags = (Revision::kLeaf | Revision::kDeleted | Revision::kHasAttachments),
            kHasBodyOffset = 0x40,  /**< Does this raw rev have a file position (oldBodyOffset)? */
            kHasData       = 0x80,  /**< Does this raw rev contain JSON data? */
        };
        
        uint32_t        size;           // Total size of this tree rev
        uint16_t        parentIndex;
        uint16_t        deltaRefIndex;
        uint8_t         flags;
        uint8_t         revIDLen;
        char            revID[1];       // actual size is [revIDLen]
        // These follow the revID:
        // varint       sequence
        // if HasData flag:
        //    char      data[];         // Contains the revision body (JSON)
        // else if HasBodyOffset flag:
        //    varint    oldBodyOffset;  // File offset of doc that has the body

        bool isValid() const {
            return size != 0;
        }

        const RawRevision *next() const {
            return (const RawRevision*)offsetby(this, ntohl(size));
        }

        unsigned count() const {
            unsigned count = 0;
            for (const RawRevision *rev = this; rev->isValid(); rev = rev->next())
                ++count;
            return count;
        }
    };


    RevTree::RevTree()
    :_bodyOffset(0), _sorted(true), _changed(false), _unknown(false)
    {}

    RevTree::RevTree(slice raw_tree, sequence seq, uint64_t docOffset)
    :_bodyOffset(docOffset), _sorted(true), _changed(false), _unknown(false)
    {
        decode(raw_tree, seq, docOffset);
    }

    RevTree::~RevTree() {
    }

    void RevTree::decode(forestdb::slice raw_tree, sequence seq, uint64_t docOffset) {
        const RawRevision *rawRev = (const RawRevision*)raw_tree.buf;
        unsigned count = rawRev->count();
        if (count > UINT16_MAX)
            throw error(error::CorruptRevisionData);
        _bodyOffset = docOffset;
        _revs.resize(count);
        auto rev = _revs.begin();
        for (; rawRev->isValid(); rawRev = rawRev->next()) {
            rev->read(rawRev);
            if (rev->sequence == 0)
                rev->sequence = seq;
            rev->owner = this;
            rev++;
        }
        if ((uint8_t*)rawRev != (uint8_t*)raw_tree.end() - sizeof(uint32_t)) {
            throw error(error::CorruptRevisionData);
        }
    }

    alloc_slice RevTree::encode() {
        sort();

        // Allocate output buffer:
        size_t size = sizeof(uint32_t);  // start with space for trailing 0 size
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev)
            size += rev->sizeToWrite();
        alloc_slice result(size);

        // Write the raw revs:
        RawRevision *dst = (RawRevision*)result.buf;
        for (auto src = _revs.begin(); src != _revs.end(); ++src) {
            dst = src->write(dst, _bodyOffset);
        }
        dst->size = htonl(0);   // write trailing 0 size marker
        assert((&dst->size + 1) == result.end());
        return result;
    }

    size_t Revision::sizeToWrite() const {
        size_t size = offsetof(RawRevision, revID) + this->revID.size + SizeOfVarInt(this->sequence);
        if (this->body.size > 0)
            size += this->body.size;
        else if (this->oldBodyOffset > 0)
            size += SizeOfVarInt(this->oldBodyOffset);
        return size;
    }

    RawRevision* Revision::write(RawRevision* dst, uint64_t bodyOffset) const {
        size_t revSize = this->sizeToWrite();
        dst->size = htonl((uint32_t)revSize);
        dst->revIDLen = (uint8_t)this->revID.size;
        memcpy(dst->revID, this->revID.buf, this->revID.size);
        dst->parentIndex = htons(this->parentIndex);
        dst->deltaRefIndex = htons(this->deltaRefIndex);

        uint8_t dstFlags = this->flags & RawRevision::kPublicPersistentFlags;
        if (this->body.size > 0)
            dstFlags |= RawRevision::kHasData;
        else if (this->oldBodyOffset > 0)
            dstFlags |= RawRevision::kHasBodyOffset;
        dst->flags = (Revision::Flags)dstFlags;

        void *dstData = offsetby(&dst->revID[0], this->revID.size);
        dstData = offsetby(dstData, PutUVarInt(dstData, this->sequence));
        if (dst->flags & RawRevision::kHasData) {
            memcpy(dstData, this->body.buf, this->body.size);
        } else if (dst->flags & RawRevision::kHasBodyOffset) {
            /*dstData +=*/ PutUVarInt(dstData, this->oldBodyOffset ?: bodyOffset);
        }

        return (RawRevision*)offsetby(dst, revSize);
    }

    void Revision::read(const RawRevision *src) {
        const void* end = src->next();
        this->revID.buf = (char*)src->revID;
        this->revID.size = src->revIDLen;
        this->flags = (Flags)(src->flags & RawRevision::kPublicPersistentFlags);
        this->parentIndex = ntohs(src->parentIndex);
        this->deltaRefIndex = ntohs(src->deltaRefIndex);
        const void *data = offsetby(&src->revID, src->revIDLen);
        ptrdiff_t len = (uint8_t*)end-(uint8_t*)data;
        data = offsetby(data, GetUVarInt(slice(data, len), &this->sequence));
        this->oldBodyOffset = 0;
        if (src->flags & RawRevision::kHasData) {
            this->body.buf = (char*)data;
            this->body.size = (char*)end - (char*)data;
        } else {
            this->body.buf = NULL;
            this->body.size = 0;
            if (src->flags & RawRevision::kHasBodyOffset) {
                slice buf = {(void*)data, (size_t)((uint8_t*)end-(uint8_t*)data)};
                GetUVarInt(buf, &this->oldBodyOffset);
            }
        }
    }

#if DEBUG
    void Revision::dump(std::ostream& out) {
        out << "(" << sequence << ") " << (std::string)revID.expanded() << "  ";
        if (isLeaf())
            out << " leaf";
        if (isDeleted())
            out << " del";
        if (hasAttachments())
            out << " attachments";
        if (isNew())
            out << " (new)";
    }
#endif

#pragma mark - ACCESSORS:

    const Revision* RevTree::currentRevision() {
        assert(!_unknown);
        sort();
        return &_revs[0];
    }

    const Revision* RevTree::get(unsigned index) const {
        assert(!_unknown);
        assert(index < _revs.size());
        return &_revs[index];
    }

    const Revision* RevTree::get(revid revID) const {
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->revID == revID)
                return &*rev;
        }
        assert(!_unknown);
        return NULL;
    }

    const Revision* RevTree::getBySequence(sequence seq) const {
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->sequence == seq)
                return &*rev;
        }
        assert(!_unknown);
        return NULL;
    }

    bool RevTree::hasConflict() const {
        if (_revs.size() < 2) {
            assert(!_unknown);
            return false;
        } else if (_sorted) {
            return _revs[1].isActive();
        } else {
            unsigned nActive = 0;
            for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
                if (rev->isActive()) {
                    if (++nActive > 1)
                        return true;
                }
            }
            return false;
        }
    }

    std::vector<const Revision*> RevTree::currentRevisions() const {
        assert(!_unknown);
        std::vector<const Revision*> cur;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->isLeaf())
                cur.push_back(&*rev);
        }
        return cur;
    }

    unsigned Revision::index() const {
        ptrdiff_t index = this - &owner->_revs[0];
        assert(index >= 0 && index < owner->_revs.size());
        return (unsigned)index;
    }

    const Revision* Revision::parent() const {
        if (parentIndex == Revision::kNoParent)
            return NULL;
        return owner->get(parentIndex);
    }

    const Revision* Revision::deltaReference() const {
        if (deltaRefIndex == Revision::kNoParent)
            return NULL;
        return owner->get(deltaRefIndex);
    }

    std::vector<const Revision*> Revision::history() const {
        std::vector<const Revision*> h;
        for (const Revision* rev = this; rev; rev = rev->parent())
            h.push_back(rev);
        return h;
    }

    bool RevTree::isBodyOfRevisionAvailable(const Revision* rev, uint64_t atOffset) const {
        return rev->body.buf != NULL; // VersionedDocument overrides this
    }

    alloc_slice RevTree::readBodyOfRevision(const Revision* rev, uint64_t atOffset) const {
        if (rev->body.buf == NULL)
            return alloc_slice();
        const Revision* referenceRev = rev->deltaReference();
        if (!referenceRev)
            return alloc_slice(rev->body);
        // Expand delta-compressed body:
        slice inlineReference = referenceRev->inlineBody();
        if (inlineReference.buf != NULL) {
            return ApplyDelta(inlineReference, rev->body, kRevDeltaFlags);
        } else {
            alloc_slice loadedReference = referenceRev->readBody();
            if (!loadedReference.buf)
                return loadedReference; // failed to read parent
            return ApplyDelta(loadedReference, rev->body, kRevDeltaFlags);
        }
    }

    // Marks a revision as a leaf, and returns true, if no revs point to it as their parent.
    bool RevTree::confirmLeaf(Revision* testRev) {
        int index = testRev->index();
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev)
            if (rev->parentIndex == index)
                return false;
        testRev->addFlag(Revision::kLeaf);
        return true;
    }

    // Returns vector mapping Revision array index to depth, with leaves at depth 0.
    // If there are branches, some revisions have ambiguous depth. If `maxDepth` is true, the
    // _longest_ path to a leaf is counted, otherwise the _shortest_ path.
    std::vector<uint16_t> RevTree::computeDepths(bool useMax) const {
        std::vector<uint16_t> depths(_revs.size(), UINT16_MAX);
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            if (rev->isLeaf()) {
                // Walk each ancestry starting from its leaf, assigning consecutive depths:
                int16_t d = 0;
                for (unsigned index = rev->index();
                              index != Revision::kNoParent;
                              index = _revs[index].parentIndex, ++d) {
                    uint16_t oldDepth = depths[index];
                    if (oldDepth == UINT16_MAX || (useMax ? d>oldDepth : d<oldDepth))
                        depths[index] = d;
                    else
                        break;
                }
            } else if (_sorted) {
                break;
            }
        }
        return depths;
    }
    

#pragma mark - INSERTION:

    // Lowest-level insert method. Does no sanity checking, always inserts.
    const Revision* RevTree::_insert(revid unownedRevID,
                                     slice body,
                                     const Revision *parentRev,
                                     bool deleted,
                                     bool hasAttachments)
    {
        assert(!_unknown);
        // Allocate copies of the revID and data so they'll stay around:
        _insertedData.push_back(alloc_slice(unownedRevID));
        revid revID = revid(_insertedData.back());
        _insertedData.push_back(alloc_slice(body));
        body = _insertedData.back();

        Revision newRev;
        newRev.owner = this;
        newRev.revID = revID;
        newRev.body = body;
        newRev.sequence = 0; // Sequence is unknown till doc is saved
        newRev.oldBodyOffset = 0; // Body position is unknown till doc is saved
        newRev.flags = (Revision::Flags)(Revision::kLeaf | Revision::kNew);
        if (deleted)
            newRev.addFlag(Revision::kDeleted);
        if (hasAttachments)
            newRev.addFlag(Revision::kHasAttachments);

        newRev.parentIndex = Revision::kNoParent;
        newRev.deltaRefIndex = Revision::kNoParent;
        if (parentRev) {
            ptrdiff_t parentIndex = parentRev->index();
            newRev.parentIndex = (uint16_t)parentIndex;
            ((Revision*)parentRev)->clearFlag(Revision::kLeaf);
        }

        _revs.push_back(newRev);

        _changed = true;
        if (_revs.size() > 1)
            _sorted = false;
        return &_revs.back();
    }

    const Revision* RevTree::insert(revid revID, slice data, bool deleted, bool hasAttachments,
                                   const Revision* parent, bool allowConflict,
                                   int &httpStatus)
    {
        // Make sure the given revID is valid:
        uint32_t newGen = revID.generation();
        if (newGen == 0) {
            httpStatus = 400;
            return NULL;
        }

        if (get(revID)) {
            httpStatus = 200;
            return NULL; // already exists
        }

        // Find the parent rev, if a parent ID is given:
        uint32_t parentGen;
        if (parent) {
            if (!allowConflict && !parent->isLeaf()) {
                httpStatus = 409;
                return NULL;
            }
            parentGen = parent->revID.generation();
        } else {
            if (!allowConflict && _revs.size() > 0) {
                httpStatus = 409;
                return NULL;
            }
            parentGen = 0;
        }

        // Enforce that generation number went up by 1 from the parent:
        if (newGen != parentGen + 1) {
            httpStatus = 400;
            return NULL;
        }
        
        // Finally, insert:
        httpStatus = deleted ? 200 : 201;
        return _insert(revID, data, parent, deleted, hasAttachments);
    }

    const Revision* RevTree::insert(revid revID, slice body, bool deleted, bool hasAttachments,
                                   revid parentRevID, bool allowConflict,
                                   int &httpStatus)
    {
        const Revision* parent = NULL;
        if (parentRevID.buf) {
            parent = get(parentRevID);
            if (!parent) {
                httpStatus = 404;
                return NULL; // parent doesn't exist
            }
        }
        return insert(revID, body, deleted, hasAttachments, parent, allowConflict, httpStatus);
    }

    int RevTree::insertHistory(const std::vector<revid> history, slice data,
                               bool deleted, bool hasAttachments) {
        assert(history.size() > 0);
        // Find the common ancestor, if any. Along the way, preflight revision IDs:
        int i;
        unsigned lastGen = 0;
        const Revision* parent = NULL;
        size_t historyCount = history.size();
        for (i = 0; i < historyCount; i++) {
            unsigned gen = history[i].generation();
            if (lastGen > 0 && gen != lastGen - 1)
                return -1; // generation numbers not in sequence
            lastGen = gen;

            parent = get(history[i]);
            if (parent)
                break;
        }
        int commonAncestorIndex = i;

        if (i > 0) {
            // Insert all the new revisions in chronological order:
            while (--i > 0)
                parent = _insert(history[i], slice(), parent, false, false);
            _insert(history[0], data, parent, deleted, hasAttachments);
        }
        return commonAncestorIndex;
    }

#pragma mark - COMPRESSING / REMOVING REVISION BODIES:

    // low-level subroutine
    void RevTree::replaceBody(Revision* rev, alloc_slice& body) {
        if (body.buf) {
            _insertedData.push_back(body);
        } else {
            if (!rev->body.buf)
                return; // no-op
            assert(_bodyOffset > 0);
            rev->oldBodyOffset = _bodyOffset; // remember the offset of the doc where body existed
        }
        rev->body = body;
        _changed = true;
    }

    // Removes the body of a revision. If this revision is the delta source of another revision,
    // either return false (if allowExpansion is false), or expand the dependent revision's body.
    bool RevTree::removeBody(Revision* rev, bool allowExpansion) {
        if (!rev->body.buf)
            return true;
        // If this rev is the reference of a compressed revision, expand the target:
        unsigned index = rev->index();
        for (auto otherRev = _revs.begin(); otherRev != _revs.end(); ++otherRev) {
            if (otherRev->deltaRefIndex == index) {
                if (!allowExpansion || !decompress(&*otherRev))
                    return false;
            }
        }
        alloc_slice empty;
        replaceBody(rev, empty);
        return true;
    }

    // Generates a zdelta from `reference` to the receiver.
    alloc_slice Revision::generateZDeltaFrom(const Revision* reference) const {
        assert(reference != NULL);
        assert(reference->owner == this->owner);
        assert(reference != this);
        if (this->isCompressed() && this->deltaRefIndex == reference->index())
            return alloc_slice(this->body);
        alloc_slice targetData = this->readBody();
        alloc_slice referenceData = reference->readBody();
        if (!targetData.buf || !referenceData.buf)
            return alloc_slice();
#if 0
        alloc_slice result = CreateDelta(referenceData, targetData, kRevDeltaFlags);
        fprintf(stderr, "DELTA: %zd bytes (%lu%%) for %.*s ---> %.*s\n", result.size,
                (result.size * 100 / targetData.size),
                (int)referenceData.size, referenceData.buf,
                (int)targetData.size, targetData.buf);
        return result;
#else
        return CreateDelta(referenceData, targetData, kRevDeltaFlags);
#endif
    }

    // Applies a delta to the receiver.
    alloc_slice Revision::applyZDelta(slice delta) {
        if (body.buf) {
            if (!isCompressed()) {
                // avoid memcpy overhead by not using an alloc_slice
                return ApplyDelta(body, delta, kRevDeltaFlags);
            } else {
                alloc_slice loadedReference = readBody();
                if (loadedReference.buf)
                    return ApplyDelta(loadedReference, delta, kRevDeltaFlags);
            }
        }
        return alloc_slice(); // failed
    }

    // Replaces the body of revision `target` with a delta computed from revision `reference`
    bool RevTree::compress(Revision* target, const Revision* reference) {
        if (target->isCompressed())
            return true;
        // Make sure there won't be a cycle:
        for (const Revision* rev = reference; rev->isCompressed(); rev = rev->deltaReference())
            if (rev == target)
                return false;

        alloc_slice delta = target->generateZDeltaFrom(reference);
        if (!delta.buf)
            return false;

        replaceBody(target, delta);
        target->deltaRefIndex = (uint16_t)reference->index();
        return true;
    }

    // If the body of `rev` is a delta, expands it and stores the expanded body.
    bool RevTree::decompress(Revision* rev) {
        if (!rev->isCompressed())
            return true;
        alloc_slice body = rev->readBody();
        if (!body.buf)
            return false;
        replaceBody(rev, body);
        rev->deltaRefIndex = Revision::kNoParent;
        return true;
    }

    // Completely removes all revisions more than `maxDepth` away from a leaf revision.
    unsigned RevTree::prune(unsigned maxDepth) {
        if (maxDepth == 0 || _revs.size() <= maxDepth)
            return 0;

        auto depths = computeDepths(true);
        unsigned numPruned = 0;
        for (unsigned i=0; i<_revs.size(); i++) {
            if (depths[i] > maxDepth) {
                _revs[i].revID.size = 0;
                numPruned++;
            }
        }
        if (numPruned > 0)
            compact();
        return numPruned;
    }

    // Completely removes a revision given by ID, and all its ancestors.
    int RevTree::purge(revid leafID) {
        int nPurged = 0;
        Revision* rev = (Revision*)get(leafID);
        if (!rev || !rev->isLeaf())
            return 0;
        do {
            nPurged++;
            rev->revID.size = 0;                    // mark for purge
            const Revision* parent = (Revision*)rev->parent();
            rev->parentIndex = Revision::kNoParent; // unlink from parent
            rev = (Revision*)parent;
        } while (rev && confirmLeaf(rev));
        compact();
        return nPurged;
    }

    // Subroutine of prune/purge that slides surviving revisions down the _revs array.
    void RevTree::compact() {
        // Create a mapping from current to new rev indexes (after removing pruned/purged revs)
        uint16_t map[_revs.size()];
        unsigned i = 0, j = 0;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev, ++i) {
            if (rev->revID.size > 0)
                map[i] = (uint16_t)(j++);
            else
                map[i] = Revision::kNoParent;
        }

        // Finally, slide the surviving revs down and renumber their parent indexes:
        Revision* rev = &_revs[0];
        Revision* dst = rev;
        for (i=0; i<_revs.size(); i++,rev++) {
            if (rev->revID.size > 0) {
                if (rev->parentIndex != Revision::kNoParent)
                    rev->parentIndex = map[rev->parentIndex];
                if (rev->deltaRefIndex != Revision::kNoParent)
                    rev->deltaRefIndex = map[rev->deltaRefIndex];
                if (dst != rev)
                    *dst = *rev;
                dst++;
            }
        }
        _revs.resize(dst - &_revs[0]);
        _changed = true;
    }

    // Sort comparison function for an array of Revisions. Higher priority comes _first_.
    bool Revision::operator<(const Revision& rev2) const
    {
        // Leaf revs go first.
        int delta = rev2.isLeaf() - this->isLeaf();
        if (delta)
            return delta < 0;
        // Else non-deleted revs go first.
        delta = this->isDeleted() - rev2.isDeleted();
        if (delta)
            return delta < 0;
        // Otherwise compare rev IDs, with higher rev ID going first:
        return rev2.revID < this->revID;
    }

    // Sorts the revisions by descending revid, placing the default/winning rev at index 0.
    void RevTree::sort() {
        if (_sorted)
            return;

        // oldParents maps rev index to the original parentIndex, before the sort.
        // At the same time we change parentIndex[i] to i, so we can track what the sort did.
        uint16_t oldParents[_revs.size()];
        for (uint16_t i = 0; i < _revs.size(); ++i) {
            oldParents[i] = _revs[i].parentIndex;
            _revs[i].parentIndex = i;
        }

        std::sort(_revs.begin(), _revs.end());

        // oldToNew maps old array indexes to new (sorted) ones.
        uint16_t oldToNew[_revs.size()];
        for (uint16_t i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i].parentIndex;
            oldToNew[oldIndex] = i;
        }

        // Now fix up the parentIndex values by running them through oldToNew:
        for (unsigned i = 0; i < _revs.size(); ++i) {
            uint16_t oldIndex = _revs[i].parentIndex;
            uint16_t parent = oldParents[oldIndex];
            if (parent != Revision::kNoParent)
                parent = oldToNew[parent];
            _revs[i].parentIndex = parent;

            uint16_t deltaRefIndex = _revs[i].deltaRefIndex;
            if (deltaRefIndex != Revision::kNoParent)
                _revs[i].deltaRefIndex = oldToNew[deltaRefIndex];
        }
        _sorted = true;
    }

#if DEBUG
    std::string RevTree::dump() {
        std::stringstream out;
        dump(out);
        return out.str();
    }

    void RevTree::dump(std::ostream& out) {
        int i = 0;
        for (auto rev = _revs.begin(); rev != _revs.end(); ++rev) {
            out << "\t" << (++i) << ": ";
            rev->dump(out);
            out << "\n";
        }
    }
#endif

}
