/*
** Copyright (c) 2011 Code Aurora Forum. All rights reserved.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

/*#error uncomment this for compiler test!*/

//#define LOG_NDEBUG 0
#define LOG_NIDEBUG 0
#define LOG_TAG "QCameraHWI_Mem"
#include <utils/Log.h>

#include <utils/Errors.h>
#include <utils/threads.h>
#include <binder/MemoryHeapPmem.h>
#ifdef USE_ION
#include <binder/MemoryHeapIon.h>
#endif
#include <utils/String16.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cutils/properties.h>
#include <math.h>
#if HAVE_ANDROID_OS
#include <linux/android_pmem.h>
#endif
#include <linux/ioctl.h>
#include <camera/CameraParameters.h>
#include <media/mediarecorder.h>
#include <gralloc_priv.h>

#include "QCameraHWI_Mem.h"

#define CAMERA_HAL_UNUSED(expr) do { (void)(expr); } while (0)

/* QCameraHardwareInterface class implementation goes here*/
/* following code implement the contol logic of this class*/

namespace android {


static bool register_buf(int size,
                         int frame_size,
                         int cbcr_offset,
                         int yoffset,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         int pmem_type,
                         bool vfe_can_write,
                         bool register_buffer = true);

#if 0
MMCameraDL::MMCameraDL(){
    LOGV("MMCameraDL: E");
    libmmcamera = NULL;
#if DLOPEN_LIBMMCAMERA
    libmmcamera = ::dlopen("liboemcamera.so", RTLD_NOW);
#endif
    LOGV("Open MM camera DL libeomcamera loaded at %p ", libmmcamera);
    LOGV("MMCameraDL: X");
}

void * MMCameraDL::pointer(){
    return libmmcamera;
}

MMCameraDL::~MMCameraDL(){
    LOGV("~MMCameraDL: E");
    LINK_mm_camera_destroy();
    if (libmmcamera != NULL) {
        ::dlclose(libmmcamera);
        LOGV("closed MM Camera DL ");
    }
    libmmcamera = NULL;
    LOGV("~MMCameraDL: X");
}


wp<MMCameraDL> MMCameraDL::instance;
Mutex MMCameraDL::singletonLock;


sp<MMCameraDL> MMCameraDL::getInstance(){
    Mutex::Autolock instanceLock(singletonLock);
    sp<MMCameraDL> mmCamera = instance.promote();
    if(mmCamera == NULL){
        mmCamera = new MMCameraDL();
        instance = mmCamera;
    }
    return mmCamera;
}
#endif

MemPool::MemPool(int buffer_size, int num_buffers,
                                         int frame_size,
                                         const char *name) :
    mBufferSize(buffer_size),
    mNumBuffers(num_buffers),
    mFrameSize(frame_size),
    mBuffers(NULL), mName(name)
{
    int page_size_minus_1 = getpagesize() - 1;
    mAlignedBufferSize = (buffer_size + page_size_minus_1) & (~page_size_minus_1);
}

void MemPool::completeInitialization()
{
    // If we do not know how big the frame will be, we wait to allocate
    // the buffers describing the individual frames until we do know their
    // size.

    if (mFrameSize > 0) {
        mBuffers = new sp<MemoryBase>[mNumBuffers];
        for (int i = 0; i < mNumBuffers; i++) {
            mBuffers[i] = new
                MemoryBase(mHeap,
                           i * mAlignedBufferSize,
                           mFrameSize);
        }
    }
}

AshmemPool::AshmemPool(int buffer_size, int num_buffers,
                                               int frame_size,
                                               const char *name) :
    MemPool(buffer_size,
                                    num_buffers,
                                    frame_size,
                                    name)
{
    LOGV("constructing MemPool %s backed by ashmem: "
         "%d frames @ %d uint8_ts, "
         "buffer size %d",
         mName,
         num_buffers, frame_size, buffer_size);

    int page_mask = getpagesize() - 1;
    int ashmem_size = buffer_size * num_buffers;
    ashmem_size += page_mask;
    ashmem_size &= ~page_mask;

    mHeap = new MemoryHeapBase(ashmem_size);

    completeInitialization();
}

static bool register_buf(int size,
                         int frame_size,
                         int cbcr_offset,
                         int yoffset,
                         int pmempreviewfd,
                         uint32_t offset,
                         uint8_t *buf,
                         int pmem_type,
                         bool vfe_can_write,
                         bool register_buffer)
{
    struct msm_pmem_info pmemBuf;
    CAMERA_HAL_UNUSED(frame_size);

    pmemBuf.type     = pmem_type;
    pmemBuf.fd       = pmempreviewfd;
    pmemBuf.offset   = offset;
    pmemBuf.len      = size;
    pmemBuf.vaddr    = buf;
    pmemBuf.y_off    = yoffset;
    pmemBuf.cbcr_off = cbcr_offset;

    pmemBuf.active   = vfe_can_write;

    LOGV("register_buf:  reg = %d buffer = %p",
         !register_buffer, buf);
    /*TODO*/
    /*if(native_start_ops(register_buffer ? CAMERA_OPS_REGISTER_BUFFER :
        CAMERA_OPS_UNREGISTER_BUFFER ,(void *)&pmemBuf) < 0) {
         LOGE("register_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM  error %s",
               strerror(errno));
         return false;
         }*/

    return true;

}

#if 0
bool register_record_buffers(bool register_buffer) {
    LOGI("%s: (%d) E", __FUNCTION__, register_buffer);
    struct msm_pmem_info pmemBuf;

    for (int cnt = 0; cnt < VIDEO_BUFFER_COUNT; ++cnt) {
        pmemBuf.type     = MSM_PMEM_VIDEO;
        pmemBuf.fd       = mRecordHeap->mHeap->getHeapID();
        pmemBuf.offset   = mRecordHeap->mAlignedBufferSize * cnt;
        pmemBuf.len      = mRecordHeap->mBufferSize;
        pmemBuf.vaddr    = (uint8_t *)mRecordHeap->mHeap->base() + mRecordHeap->mAlignedBufferSize * cnt;
        pmemBuf.y_off    = 0;
        pmemBuf.cbcr_off = recordframes[0].cbcr_off;
        if(register_buffer == true) {
            pmemBuf.active   = (cnt<ACTIVE_VIDEO_BUFFERS);
            if( (mVpeEnabled) && (cnt == kRecordBufferCount-1)) {
                pmemBuf.type = MSM_PMEM_VIDEO_VPE;
                pmemBuf.active = 1;
            }
        } else {
            pmemBuf.active   = false;
        }

        LOGV("register_buf:  reg = %d buffer = %p", !register_buffer,
          (void *)pmemBuf.vaddr);
        if(native_start_ops(register_buffer ? CAMERA_OPS_REGISTER_BUFFER :
                CAMERA_OPS_UNREGISTER_BUFFER ,(void *)&pmemBuf) < 0) {
            LOGE("register_buf: MSM_CAM_IOCTL_(UN)REGISTER_PMEM  error %s",
                strerror(errno));
            return false;
        }
    }
    return true;
}
#endif
PmemPool::PmemPool(const char *pmem_pool,
                                           int flags,
                                           int pmem_type,
                                           int buffer_size, int num_buffers,
                                           int frame_size, int cbcr_offset,
                                           int yOffset, const char *name) :
    MemPool(buffer_size,num_buffers,frame_size,name),
    mPmemType(pmem_type),
    mCbCrOffset(cbcr_offset),
    myOffset(yOffset)
{
    LOGI("constructing MemPool %s backed by pmem pool %s: "
         "%d frames @ %d bytes, buffer size %d",
         mName,
         pmem_pool, num_buffers, frame_size,
         buffer_size);

    //mMMCameraDLRef = MMCameraDL::getInstance();


    // Make a new mmap'ed heap that can be shared across processes.
    // mAlignedBufferSize is already in 4k aligned. (do we need total size necessary to be in power of 2??)
    mAlignedSize = mAlignedBufferSize * num_buffers;

    sp<MemoryHeapBase> masterHeap =
        new MemoryHeapBase(pmem_pool, mAlignedSize, flags);

    if (masterHeap->getHeapID() < 0) {
        LOGE("failed to construct master heap for pmem pool %s", pmem_pool);
        masterHeap.clear();
        return;
    }

    sp<MemoryHeapPmem> pmemHeap = new MemoryHeapPmem(masterHeap, flags);
    if (pmemHeap->getHeapID() >= 0) {
        pmemHeap->slap();
        masterHeap.clear();
        mHeap = pmemHeap;
        pmemHeap.clear();

        mFd = mHeap->getHeapID();
        if (::ioctl(mFd, PMEM_GET_SIZE, &mSize)) {
            LOGE("pmem pool %s ioctl(PMEM_GET_SIZE) error %s (%d)",
                 pmem_pool,
                 ::strerror(errno), errno);
            mHeap.clear();
            return;
        }

        LOGE("pmem pool %s ioctl(fd = %d, PMEM_GET_SIZE) is %ld",
             pmem_pool,
             mFd,
             mSize.len);
        LOGE("mBufferSize=%d, mAlignedBufferSize=%d\n", mBufferSize, mAlignedBufferSize);

#if 0
        // Unregister preview buffers with the camera drivers.  Allow the VFE to write
        // to all preview buffers except for the last one.
        // Only Register the preview, snapshot and thumbnail buffers with the kernel.
        if( (strcmp("postview", mName) != 0) ){
            int num_buf = num_buffers;
            if(!strcmp("preview", mName)) num_buf = kPreviewBufferCount;
            LOGD("num_buffers = %d", num_buf);
            for (int cnt = 0; cnt < num_buf; ++cnt) {
                int active = 1;
                if(pmem_type == MSM_PMEM_VIDEO){
                     active = (cnt<ACTIVE_VIDEO_BUFFERS);
                     //When VPE is enabled, set the last record
                     //buffer as active and pmem type as PMEM_VIDEO_VPE
                     //as this is a requirement from VPE operation.
                     //No need to set this pmem type to VIDEO_VPE while unregistering,
                     //because as per camera stack design: "the VPE AXI is also configured
                     //when VFE is configured for VIDEO, which is as part of preview
                     //initialization/start. So during this VPE AXI config camera stack
                     //will lookup the PMEM_VIDEO_VPE buffer and give it as o/p of VPE and
                     //change it's type to PMEM_VIDEO".
                     if( (mVpeEnabled) && (cnt == kRecordBufferCount-1)) {
                         active = 1;
                         pmem_type = MSM_PMEM_VIDEO_VPE;
                     }
                     LOGV(" pmempool creating video buffers : active %d ", active);
                }
                else if (pmem_type == MSM_PMEM_PREVIEW){
                    active = (cnt < ACTIVE_PREVIEW_BUFFERS);
                }
                else if ((pmem_type == MSM_PMEM_MAINIMG)
                     || (pmem_type == MSM_PMEM_THUMBNAIL)){
                    active = (cnt < ACTIVE_ZSL_BUFFERS);
                }
                register_buf(mBufferSize,
                         mFrameSize, mCbCrOffset, myOffset,
                         mHeap->getHeapID(),
                         mAlignedBufferSize * cnt,
                         (uint8_t *)mHeap->base() + mAlignedBufferSize * cnt,
                         pmem_type,
                         active);
            }
        }
#endif
        completeInitialization();
    }
    else LOGE("pmem pool %s error: could not create master heap!",
              pmem_pool);
    LOGI("%s: (%s) X ", __FUNCTION__, mName);
}

PmemPool::~PmemPool()
{
    LOGI("%s: %s E", __FUNCTION__, mName);
#if 0
    if (mHeap != NULL) {
        // Unregister preview buffers with the camera drivers.
        //  Only Unregister the preview, snapshot and thumbnail
        //  buffers with the kernel.
        if( (strcmp("postview", mName) != 0) ){
            int num_buffers = mNumBuffers;
            if(!strcmp("preview", mName)) num_buffers = PREVIEW_BUFFER_COUNT;
            for (int cnt = 0; cnt < num_buffers; ++cnt) {
                register_buf(mBufferSize,
                         mFrameSize,
                         mCbCrOffset,
                         myOffset,
                         mHeap->getHeapID(),
                         mAlignedBufferSize * cnt,
                         (uint8_t *)mHeap->base() + mAlignedBufferSize * cnt,
                         mPmemType,
                         false,
                         false /* unregister */);
            }
        }
    }
    mMMCameraDLRef.clear();
#endif
    LOGI("%s: %s X", __FUNCTION__, mName);
}
#ifdef USE_ION
const char IonPool::mIonDevName[] = "/dev/ion";

IonPool::IonPool(int flags,
                 int buffer_size,
                 int num_buffers,
                 int frame_size,
                 int cbcr_offset,
                 int yOffset,
                 const char *name) :
    MemPool(buffer_size,num_buffers,frame_size,name),
    mCbCrOffset(cbcr_offset),
    myOffset(yOffset)
{
    LOGI("constructing MemPool %s backed by ion pool %s: "
         "%d frames @ %d bytes, buffer size %d",
         mName,
         mIonDevName, num_buffers, frame_size,
         buffer_size);

    // Make a new mmap'ed heap that can be shared across processes.
    // mAlignedBufferSize is already in 4k aligned.
    mAlignedSize = mAlignedBufferSize * num_buffers;
    sp<MemoryHeapIon> ionHeap = new MemoryHeapIon(mIonDevName, mAlignedSize,
                                                  flags, 0x1<<ION_HEAP_ADSP_ID);
    if (ionHeap->getHeapID() >= 0) {
        mHeap = ionHeap;
        ionHeap.clear();

        mFd = mHeap->getHeapID();
        LOGE("ion pool %s fd = %d", mIonDevName, mFd);
        LOGE("mBufferSize=%d, mAlignedBufferSize=%d\n",
                      mBufferSize, mAlignedBufferSize);
        completeInitialization();
    }
    else
        LOGE("ion pool %s error: could not create master heap!", mIonDevName);
    LOGI("%s: (%s) X ", __FUNCTION__, mName);
}

IonPool::~IonPool()
{
    LOGI("%s: %s E", __FUNCTION__, mName);
    LOGI("%s: %s X", __FUNCTION__, mName);
}
#endif
MemPool::~MemPool()
{
    LOGV("destroying MemPool %s", mName);
    if (mFrameSize > 0)
        delete [] mBuffers;
    mHeap.clear();
    LOGV("destroying MemPool %s completed", mName);
}


status_t MemPool::dump(int fd, const Vector<String16>& args) const
{
    const size_t SIZE = 256;
    char buffer[SIZE];
    String8 result;
    CAMERA_HAL_UNUSED(args);
    snprintf(buffer, 255, "QualcommCameraHardware::AshmemPool::dump\n");
    result.append(buffer);
    if (mName) {
        snprintf(buffer, 255, "mem pool name (%s)\n", mName);
        result.append(buffer);
    }
    if (mHeap != 0) {
        snprintf(buffer, 255, "heap base(%p), size(%d), flags(%d), device(%s)\n",
                 mHeap->getBase(), mHeap->getSize(),
                 mHeap->getFlags(), mHeap->getDevice());
        result.append(buffer);
    }
    snprintf(buffer, 255,
             "buffer size (%d), number of buffers (%d), frame size(%d)",
             mBufferSize, mNumBuffers, mFrameSize);
    result.append(buffer);
    write(fd, result.string(), result.size());
    return NO_ERROR;
}

};
