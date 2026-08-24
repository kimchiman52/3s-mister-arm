#ifndef PORT_IO_AFS_H
#define PORT_IO_AFS_H

#include <stdbool.h>

typedef enum AFSReadState {
    AFS_READ_STATE_IDLE,
    AFS_READ_STATE_READING,
    AFS_READ_STATE_FINISHED,
    AFS_READ_STATE_ERROR
} AFSReadState;

typedef int AFSHandle;

#define AFS_NONE -1

bool AFS_Init(const char* file_path);
void AFS_Finish();
unsigned int AFS_GetFileCount();
unsigned int AFS_GetSize(int file_num);

/// Synchronous byte-range read of one archive member: reads `size` bytes
/// starting `offset` bytes into file `file_num`, independent of the async
/// request-slot machinery above. Used by the boot-time arcade-balance
/// adaptation to read only each character's PS2 char-data tail instead of
/// whole multi-megabyte texture-group files. Returns false on any
/// out-of-range or I/O failure.
bool AFS_ReadRange(int file_num, unsigned int offset, unsigned int size, void* buf);

void AFS_RunServer();
AFSHandle AFS_Open(int file_num);
void AFS_Read(AFSHandle handle, int sectors, void* buf);
void AFS_ReadSync(AFSHandle handle, int sectors, void* buf);
void AFS_Stop(AFSHandle handle);
void AFS_Close(AFSHandle handle);
AFSReadState AFS_GetState(AFSHandle handle);
unsigned int AFS_GetSectorCount(AFSHandle handle);

#endif
