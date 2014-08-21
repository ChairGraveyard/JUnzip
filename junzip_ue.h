// Callback prototype for central and local file record reading functions
typedef int (*JZRecordCallback)(FILE *zip, int index, JZFileHeader *header, char *filename);

#define JZ_BUFFER_SIZE 65536

// Read ZIP file end record. Will move within file.
int jzReadEndRecord(const FString& FilePath,, JZEndRecord *endRecord, FArchive* zip);
