# graf
A Gzip Random Access Facilitator based on zran.c


## Usage
  Create an index: `graf (gzfile) (idxfile) build (spanlength)`  
  Use an index to read uncompressed data: `graf (gzfile) (idxfile) read (offset) (bytes to read)`

## Examples
### 1. Generate an index file
You only have to do this once for a given .gz file:

  `graf myfile.gz myfile.gz.idx build 1048576`

  - This does a full read of myfile.gz to create an index file with 1MB spans, named myfile.gz.idx
  - Larger span lengths decrease index filesize, but cause longer random access times and require more memory to work with.
  - Each index entry is approximately 32KB in size, so a span length less than that would just be silly.

### 2. Use the index file:

  `./graf myfile.gz myfile.gz.idx read 9000 42`

  - This uses the index file you generated in step 1 to read the 42 bytes that begin at uncompressed offset 9000.
  - This (and all subsequent) reads will be much faster than having to decompress the whole file up to your desired offset.
  - Attempted reads which extend beyond the end of the file and/or begin at non-existent offsets may result in errors...or segfaults.
