#include "junzip_ue.h"

// Source

unsigned char jzBuffer[JZ_BUFFER_SIZE]; // limits maximum zip descriptor size

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(const FString& FilePath, JZEndRecord *endRecord, FArchive* zip) 
{
    long fileSize, readBytes, i;
    JZEndRecord *er;

	TAutoPtr<FArchive> FileReader( IFileManager::Get().CreateFileReader( *FilePath ) );
	if( !FileReader )
	{
		UE_LOG( LogStats, Error, TEXT( "Could not open: %s" ), *FilePath );
		return Z_ERRNO;
	}

	if (!FileReader->Seek(FileReader->Size()))
	{
		UE_LOG( LogStats, Error, TEXT( "Couldn't go to end of zip file!" ) );
	    return Z_ERRNO;
	}

	if((fileSize = FileReader->Tell()) <= sizeof(JZEndRecord)) 
	{
        UE_LOG( LogStats, Error, TEXT( "Too small file to be a zip!" ) );
        return Z_ERRNO;
    }

    readBytes = (fileSize < sizeof(jzBuffer)) ? fileSize : sizeof(jzBuffer);

	if (!FileReader->Seek())
	{
		UE_LOG( LogStats, Error, TEXT( "Couldn't go to beginning of zip file!" ) );
	    return Z_ERRNO;
	}

	if (!FileReader->Seek(fileSize - readBytes))
	{
		UE_LOG( LogStats, Error, TEXT( "Cannot seek in zip file!" ) );
	    return Z_ERRNO;
	}

	if(!FileReader->Read(jzBuffer, readBytes))
	{
		UE_LOG( LogStats, Error, TEXT( "Couldn't read end of zip file!" ) );
	    return Z_ERRNO;
	}	

    // Naively assume signature can only be found in one place...
    for(i = readBytes - sizeof(JZEndRecord); i >= 0; i--) {
        er = (JZEndRecord *)(jzBuffer + i);
        if(er->signature == 0x06054B50)
            break;
    }

    if(i < 0) 
    {
        UE_LOG( LogStats, Error, TEXT( "End record signature not found in zip!" ) );
        return Z_ERRNO;
    }

    FMemory::Memcpy(endRecord, er, sizeof(JZEndRecord));

    if(endRecord->diskNumber || endRecord->centralDirectoryDiskNumber ||
            endRecord->numEntries != endRecord->numEntriesThisDisk) 
   	{
        UE_LOG( LogStats, Error, TEXT( "Multifile zips not supported!" ) );
        return Z_ERRNO;
    }

    zip = FileReader;

    return Z_OK;
}

// Read ZIP file global directory. Will move within file.
int jzReadCentralDirectory(FArchive *zip, JZEndRecord *endRecord,
        JZRecordCallback callback) {
    JZGlobalFileHeader fileHeader;
    JZFileHeader header;
    int i;

    if(!zip->Seek(0)))
	{
		UE_LOG( LogStats, Error, TEXT( "Cannot seek in zip file!" ) );
	    return Z_ERRNO;
	}

    if(!zip->Seek(endRecord->centralDirectoryOffset)))
	{
		UE_LOG( LogStats, Error, TEXT( "Cannot seek in zip file!" ) );
	    return Z_ERRNO;
	}

    for(i=0; i<endRecord->numEntries; i++) 
    {	    
	    if(!zip->Read(&fileHeader, sizeof(JZGlobalFileHeader)))
	    {
	        UE_LOG( LogStats, Error, TEXT( "Couldn't read file header %d!" ), i );
	        return Z_ERRNO;
	    }

        if(fileHeader.signature != 0x02014B50) 
        {
        	UE_LOG( LogStats, Error, TEXT( "Invalid file header signature %d!" ), i );
            return Z_ERRNO;
        }

        if(fileHeader.fileNameLength + 1 >= JZ_BUFFER_SIZE) 
        {
        	UE_LOG( LogStats, Error, TEXT( "Too long file name %d!" ), i );
            return Z_ERRNO;
        }

    	if(!zip->Read(jzBuffer, fileHeader.fileNameLength))
	    {
	        UE_LOG( LogStats, Error, TEXT( "Couldn't read filename %d!" ), i );
	        return Z_ERRNO;
	    }

        jzBuffer[fileHeader.fileNameLength] = '\0'; // NULL terminate

	    if(!zip->Seek(fileHeader.extraFieldLength)) || !zip->Seek(fileHeader.fileCommentLength)))
		{
			UE_LOG( LogStats, Error, TEXT( "Couldn't skip extra field or file comment %d" ), i );
		    return Z_ERRNO;
		}    	

        // Construct JZFileHeader from global file header
        FMemory::Memcpy(&header, &fileHeader.compressionMethod, sizeof(header));
        header.offset = fileHeader.relativeOffsetOflocalHeader;

        if(!callback(zip, i, &header, (char *)jzBuffer))
            break; // end if callback returns zero
    }

    return Z_OK;
}

// Read local ZIP file header. Silent on errors so optimistic reading possible.
int jzReadLocalFileHeader(FArchive *zip, JZFileHeader *header, char *filename, int len) 
{
    JZLocalFileHeader localHeader;

    if(!zip->Read(%localHeader, sizeof(JZLocalFileHeader)))
    	return Z_ERRNO;

    if(localHeader.signature != 0x04034B50)
        return Z_ERRNO;

    if(len) 
    { // read filename
        if(localHeader.fileNameLength >= len)
            return Z_ERRNO; // filename cannot fit

    	if (!zip->Read(filename, localHeader.fileNameLength))
    		return Z_ERRNO;

        filename[localHeader.fileNameLength] = '\0'; // NULL terminate
    } 
    else 
    { 
    	// skip filename
    	if(!zip->Seek(localHeader.fileNameLength))
    		return Z_ERRNO;    	
    }

    if(localHeader.extraFieldLength) 
    {
    	if (!zip->Seek(localHeader.extraFieldLength))
    		return Z_ERRNO;    	
    }

    if(localHeader.generalPurposeBitFlag)
        return Z_ERRNO; // Flags not supported

    if(localHeader.compressionMethod == 0 &&
            (localHeader.compressedSize != localHeader.uncompressedSize))
        return Z_ERRNO; // Method is "store" but sizes indicate otherwise, abort

    FMemory::Memcpy(header, &localHeader.compressionMethod, sizeof(JZFileHeader));    
    header->offset = 0; // not used in local context

    return Z_OK;
}

// Read data from file stream, described by header, to preallocated buffer
int jzReadData(FArchive *zip, JZFileHeader *header, void *buffer) {
    unsigned char *bytes = (unsigned char *)buffer; // cast
    long compressedLeft, uncompressedLeft;
    z_stream strm;
    int ret;

    if(header->compressionMethod == 0) 
    { // Store - just read it
    	if (!zip->Read(buffer, header->uncompressedSize))
    		return Z_ERRNO;
    } else if(header->compressionMethod == 8) { // Deflate - using zlib
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;

        strm.avail_in = 0;
        strm.next_in = Z_NULL;

        // Use inflateInit2 with negative window bits to indicate raw data
        if((ret = inflateInit2(&strm, -MAX_WBITS)) != Z_OK)
            return ret; // Zlib errors are negative

        // Inflate compressed data
        for(compressedLeft = header->compressedSize,
                uncompressedLeft = header->uncompressedSize;
                compressedLeft && uncompressedLeft && ret != Z_STREAM_END;
                compressedLeft -= strm.avail_in) {
            
            long cnt = (sizeof(jzBuffer) < compressedLeft) ? sizeof(jzBuffer) : compressedLeft;
        	strm.avail_in = cnt;
        	bool read_success = zip->Read(jzBuffer, cnt);    

            if(strm.avail_in == 0 || !read_success) 
            {
                inflateEnd(&strm);
                return Z_ERRNO;
            }

            strm.next_in = jzBuffer;
            strm.avail_out = uncompressedLeft;
            strm.next_out = bytes;

            compressedLeft -= strm.avail_in; // inflate will change avail_in

            ret = inflate(&strm, Z_NO_FLUSH);

            if(ret == Z_STREAM_ERROR) return ret; // shouldn't happen

            switch (ret) {
                case Z_NEED_DICT:
                    ret = Z_DATA_ERROR;     /* and fall through */
                case Z_DATA_ERROR: case Z_MEM_ERROR:
                    (void)inflateEnd(&strm);
                    return ret;
            }

            bytes += uncompressedLeft - strm.avail_out; // bytes uncompressed
            uncompressedLeft = strm.avail_out;
        }

        inflateEnd(&strm);
    } else {
        return Z_ERRNO;
    }

    return Z_OK;
}
