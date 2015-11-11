//
//  Writer.hh
//  CBForest
//
//  Created by Jens Alfke on 2/4/15.
//  Copyright (c) 2015 Couchbase. All rights reserved.
//

#ifndef __CBForest__writer__
#define __CBForest__writer__

#include "slice.hh"

namespace forestdb {

    /** A simple write-only stream that buffers its output into a slice.
        (Used instead of C++ ostreams because those have too much overhead.) */
    class Writer {
    public:
        static const size_t kDefaultInitialCapacity = 256;

        Writer(size_t initialCapacity =kDefaultInitialCapacity);
        Writer(const Writer&);
        Writer(Writer&&);
        ~Writer();

        size_t length() const                   {return _buffer.size - _available.size;}

        /** Returns the data written, without copying.
            The returned slice becomes invalid as soon as any more data is written. */
        slice output() const                    {return slice(_buffer.buf, length());}
        
        /** Returns the data written. The Writer stops managing the memory; it is the caller's
            responsibility to free the returned slice's buf! */
        slice extractOutput();

        void write(const void* data, size_t length);

        Writer& operator<< (uint8_t byte)       {return operator<<(slice(&byte,1));}
        Writer& operator<< (slice s)            {write(s.buf, s.size); return *this;}

        /** Overwrites already-written data.
            @param pos  The position in the output at which to start overwriting
            @param newData  The data that replaces the old */
        void rewrite(size_t pos, slice newData);

    private:
        void growToFit(size_t);
        const Writer& operator=(const Writer&); // forbidden

        slice _buffer;
        slice _available;
    };

}

#endif /* defined(__CBForest__writer__) */