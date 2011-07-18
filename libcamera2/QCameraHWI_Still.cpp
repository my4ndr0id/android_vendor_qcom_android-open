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
#define LOG_TAG "QCameraHWI_Still"
#include <utils/Log.h>
#include <utils/threads.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "QCameraHAL.h"
#include "QCameraHWI.h"

/* following code implement the still image capture & encoding logic of this class*/
namespace android {

typedef enum {
    SNAPSHOT_STATE_ERROR,
    SNAPSHOT_STATE_UNINIT,
    SNAPSHOT_STATE_CH_ACQUIRED,
    SNAPSHOT_STATE_BUF_NOTIF_REGD,
    SNAPSHOT_STATE_BUF_INITIALIZED,
    SNAPSHOT_STATE_INITIALIZED,
    SNAPSHOT_STATE_IMAGE_CAPTURE_STRTD,
    SNAPSHOT_STATE_YUV_RECVD,
    SNAPSHOT_STATE_JPEG_ENCODING,
    SNAPSHOT_STATE_JPEG_ENCODE_DONE,

    /*Add any new state above*/
    SNAPSHOT_STATE_MAX
} snapshot_state_type_t;


//-----------------------------------------------------------------------
// Constants
//----------------------------------------------------------------------
static const int PICTURE_FORMAT_JPEG = 1;
static const int PICTURE_FORMAT_RAW = 2;
static const int ZSL_BUFFER_NUM = 8;
static const int POSTVIEW_SMALL_HEIGHT = 144;


// ---------------------------------------------------------------------------
/* static functions*/
// ---------------------------------------------------------------------------

/* TBD: Temp: to be removed*/
static pthread_mutex_t g_s_mutex;
static int g_status = 0;
static pthread_cond_t g_s_cond_v;

static void mm_app_snapshot_done()
{
  pthread_mutex_lock(&g_s_mutex);
  g_status = TRUE;
  pthread_cond_signal(&g_s_cond_v);
  pthread_mutex_unlock(&g_s_mutex);
}

static void mm_app_snapshot_wait()
{
        pthread_mutex_lock(&g_s_mutex);
        if(FALSE == g_status) pthread_cond_wait(&g_s_cond_v, &g_s_mutex);
        pthread_mutex_unlock(&g_s_mutex);
    g_status = FALSE;
}

static int mm_app_dump_snapshot_frame(struct msm_frame *frame, uint32_t len, int is_main, int loop)
{
	char bufp[128];
	int file_fdp;
	int rc = 0;

	if(is_main) {
		sprintf(bufp, "/data/bs%d.yuv", loop);
	} else {
		sprintf(bufp, "/data/bt%d.yuv", loop);
	}

	file_fdp = open(bufp, O_RDWR | O_CREAT, 0777);

	if (file_fdp < 0) {
		rc = -1;
		goto end;
	}
	write(file_fdp,
		(const void *)frame->buffer, len);
	close(file_fdp);
end:
	return rc;
}

/* Callback received when a frame is available after snapshot*/
static void snapshot_notify_cb(mm_camera_ch_data_buf_t *recvd_frame,
							   void *user_data)
{
	QCameraStream_Snapshot *pme = (QCameraStream_Snapshot *)user_data;

	LOGD("%s: E", __func__);

    if (pme != NULL) {
        pme->receiveRawPicture(recvd_frame);
    }
    else{
        LOGW("%s: Snapshot obj NULL in callback", __func__);
    }
   
    LOGD("%s: X", __func__);

}

/* Once we give frame for encoding, we get encoded jpeg image
   fragments by fragment. We'll need to store them in a buffer
   to form complete JPEG image */
static void snapshot_jpeg_fragment_cb(uint8_t *ptr,
                                      uint32_t size,
                                      void *user_data)
{
    QCameraStream_Snapshot *pme = (QCameraStream_Snapshot *)user_data;

    LOGD("%s: E",__func__);
    if (pme != NULL) {
        pme->receiveJpegFragment(ptr,size);
    }
    else
        LOGW("%s: Receive jpeg fragment cb obj Null", __func__);

    LOGD("%s: X",__func__);
}

/* This callback is received once the complete JPEG encoding is done */
static void snapshot_jpeg_cb(jpeg_event_t event, void *user_data)
{
    QCameraStream_Snapshot *pme = (QCameraStream_Snapshot *)user_data;
    LOGD("%s: E",__func__);

    if (pme != NULL) {
         pme->receiveCompleteJpegPicture(event);
    }
    else
        LOGW("%s: Receive jpeg cb Obj Null", __func__);

    mm_jpeg_encoder_join();

    //cleanup
    /* deinit only if we are done taking requested number of snapshots */
    if (pme->getSnapshotState() == SNAPSHOT_STATE_JPEG_ENCODE_DONE) {
        /* stop snapshot polling thread */
        pme->stop();
    }

    LOGD("%s: X",__func__);

}

// ---------------------------------------------------------------------------
/* private functions*/
// ---------------------------------------------------------------------------

void QCameraStream_Snapshot::
receiveJpegFragment(uint8_t *ptr, uint32_t size)
{
    LOGD("%s: E", __func__);

    memcpy(mJpegHeap->mHeap->base()+ mJpegOffset, ptr, size);
    mJpegOffset += size;

    LOGD("%s: X", __func__);
}


void QCameraStream_Snapshot::
receiveCompleteJpegPicture(jpeg_event_t event)
{
    LOGD("%s: E", __func__);

    LOGD("%s: Calling upperlayer callback to store JPEG image", __func__);
  if (mHalCamCtrl->mDataCb && 
      (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
      // The reason we do not allocate into mJpegHeap->mBuffers[offset] is
      // that the JPEG image's size will probably change from one snapshot
      // to the next, so we cannot reuse the MemoryBase object.
      sp<MemoryBase> buffer = new MemoryBase(mJpegHeap->mHeap, 0, mJpegOffset);
      mHalCamCtrl->mDataCb(CAMERA_MSG_COMPRESSED_IMAGE,
                                 buffer,
                                 mHalCamCtrl->mCallbackCookie);
        buffer = NULL;

    } else {
      LOGW("%s: JPEG callback was cancelled--not delivering image.", __func__);
    }


    //reset jpeg_offset
    mJpegOffset = 0;

    if(mJpegHeap != 0) {
        mJpegHeap.clear();
    }

    /* Before leaving check the jpeg queue. If it's not empty give the available
       frame for encoding*/
    if (!mSnapshotQueue.isEmpty()) {
        LOGD("%s: JPEG Queue not empty. Dequeue and encode.", __func__);
        mm_camera_ch_data_buf_t* buf = 
            (mm_camera_ch_data_buf_t *)mSnapshotQueue.dequeue();
        encodeDisplayAndSave(buf);
    }
    else
    {
        setSnapshotState(SNAPSHOT_STATE_JPEG_ENCODE_DONE);
    }

    /* Tell lower layer that we are done with this buffer*/
    LOGD("%s: Calling buf done", __func__);
    mmCamera->evt->buf_done(mmCamera, mCurrentFrameEncoded);

    LOGD("%s: X", __func__);
}


status_t QCameraStream_Snapshot::
configSnapshotDimension(cam_ctrl_dimension_t* dim)
{
    bool matching = true;
    cam_format_t img_format;
    status_t ret = NO_ERROR;
    uint32_t aspect_ratio;
    LOGD("%s: E", __func__);

    LOGD("%s:Passed picture size: %d X %d", __func__,
         dim->picture_width, dim->picture_height);
    LOGD("%s:Passed postview size: %d X %d", __func__,
         dim->ui_thumbnail_width, dim->ui_thumbnail_height);

    /* First check if the picture resolution is the same, if not, change it*/
    mHalCamCtrl->getPictureSize(&mPictureWidth, &mPictureHeight);
    LOGD("%s: Picture size received: %d x %d", __func__,
         mPictureWidth, mPictureHeight);

    /* Get recommended postview/thumbnail sizes based on aspect ratio */
    aspect_ratio = (uint32_t)((mPictureWidth * Q12) / mPictureHeight);
    ret = mHalCamCtrl->getThumbSizesFromAspectRatio(aspect_ratio,
                                                    &mPostviewWidth,
                                                    &mPostviewHeight);
    /* If for some reason we can't find recommended thumbnail size,
       we'll set postview size as that of preview size*/
    if (ret != NO_ERROR) {
        mHalCamCtrl->getPreviewSize(&mPostviewWidth, &mPostviewHeight);
    }

    LOGD("%s: Postview size received: %d x %d", __func__,
         mPostviewWidth, mPostviewHeight);

    matching = (mPictureWidth == dim->picture_width) &&
        (mPictureHeight == dim->picture_height);
    matching &= (dim->ui_thumbnail_width == mPostviewWidth) &&
        (dim->ui_thumbnail_height == mPostviewHeight);

    /* picture size currently set do not match with the one wanted
       by user.*/
    if (!matching) {
        dim->picture_width  = mPictureWidth;
        dim->picture_height = mPictureHeight;
        if (mPictureHeight < mPostviewHeight) {
            mPostviewHeight = POSTVIEW_SMALL_HEIGHT;
            mPostviewWidth =
            (mPostviewHeight * mPictureWidth) / mPictureHeight;
        }
        dim->ui_thumbnail_height = mThumbnailHeight = mPostviewHeight;
        dim->ui_thumbnail_width = mThumbnailWidth = mPostviewWidth;       
    }

    /* Check the image format*/
    img_format = mHalCamCtrl->getPreviewFormat();
    if (img_format) {
        matching &= (img_format == dim->main_img_format);
        if (!matching) {
            dim->main_img_format = img_format;
            dim->thumb_format = img_format;
        }
    }

    LOGD("%s: Image Sizes before set parm call: main: %dx%d thumbnail: %dx%d",
         __func__,
         dim->picture_width, dim->picture_height,
         dim->ui_thumbnail_width, dim->ui_thumbnail_height);

    if (!matching) {
        ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_DIMENSION,dim);
        if (NO_ERROR != ret) {
            LOGE("%s: error - can't config snapshot parms!", __func__);
            ret = FAILED_TRANSACTION;
            goto end;
        }    
    }
    /* set_parm will return corrected dimension based on aspect ratio and
       ceiling size */
    mPictureWidth = dim->picture_width;
    mPictureHeight = dim->picture_height;
    mPostviewHeight = mThumbnailHeight = dim->ui_thumbnail_height;
    mPostviewWidth = mThumbnailWidth = dim->ui_thumbnail_width;

    LOGD("%s: Image Format: %d", __func__, dim->main_img_format);
    LOGD("%s: Image Sizes: main: %dx%d thumbnail: %dx%d", __func__,
         dim->picture_width, dim->picture_height,
         dim->ui_thumbnail_width, dim->ui_thumbnail_height);

    
end:
    LOGD("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::
initRawSnapshotChannel(cam_ctrl_dimension_t *dim,
                       mm_camera_raw_streaming_type_t raw_stream_type)
{
    status_t ret = NO_ERROR;
    mm_camera_ch_image_fmt_parm_t fmt;
    mm_camera_channel_attr_t ch_attr;

    LOGD("%s: E", __func__);

    LOGD("%s: Acquire Raw Snapshot Channel", __func__);
    ret = mmCamera->ops->ch_acquire(mmCamera, MM_CAMERA_CH_RAW);
    if (NO_ERROR != ret) {
        LOGE("%s: Failure Acquiring Raw Snapshot Channel error =%d\n", 
             __func__, ret);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* Snapshot channel is acquired */
    setSnapshotState(SNAPSHOT_STATE_CH_ACQUIRED);

    /* Set channel attribute */
    LOGD("%s: Set Raw Snapshot Channel attribute", __func__);
    memset(&ch_attr, 0, sizeof(ch_attr));
    ch_attr.type = MM_CAMERA_CH_ATTR_RAW_STREAMING_TYPE;
    ch_attr.raw_streaming_mode = raw_stream_type;

    if( NO_ERROR !=
        mmCamera->ops->ch_set_attr(mmCamera,MM_CAMERA_CH_RAW, &ch_attr)) {
        LOGD("%s: Failure setting Raw channel attribute.", __func__);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    memset(&fmt, 0, sizeof(mm_camera_ch_image_fmt_parm_t));
    fmt.ch_type = MM_CAMERA_CH_RAW;
    fmt.def.fmt = CAMERA_BAYER_SBGGR10;
    fmt.def.dim.width = dim->raw_picture_width;
    fmt.def.dim.height = dim->raw_picture_height;


    LOGV("%s: Raw snapshot channel fmt: %d", __func__,
         fmt.def.fmt);
    LOGV("%s: Raw snapshot resolution: %dX%d", __func__,
         dim->raw_picture_width, dim->raw_picture_height);

    LOGD("%s: Set Raw Snapshot channel image format", __func__);
    ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_CH_IMAGE_FMT, &fmt);
    if (NO_ERROR != ret) {
        LOGE("%s: Set Raw Snapshot Channel format err=%d\n", __func__, ret);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    LOGD("%s: Register buffer notification. My object: %x",
         __func__, (unsigned int) this);
    (void) mmCamera->evt->register_buf_notify(mmCamera, MM_CAMERA_CH_RAW, 
                                                snapshot_notify_cb, 
                                                this);
    /* Set the state to buffer notification completed */
    setSnapshotState(SNAPSHOT_STATE_BUF_NOTIF_REGD);

end:
    if (ret != NO_ERROR) {
        handleError();
    }
    LOGE("%s: X", __func__);
    return ret;

}

status_t QCameraStream_Snapshot::
initSnapshotChannel(cam_ctrl_dimension_t *dim)
{
    status_t ret = NO_ERROR;
    mm_camera_ch_image_fmt_parm_t fmt;

    LOGD("%s: E", __func__);

    LOGD("%s: Acquire Snapshot Channel", __func__);
    ret = mmCamera->ops->ch_acquire(mmCamera, MM_CAMERA_CH_SNAPSHOT);
    if (NO_ERROR != ret) {
        LOGE("%s: Failure Acquiring Snapshot Channel error =%d\n", __func__, ret);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* Snapshot channel is acquired */
    setSnapshotState(SNAPSHOT_STATE_CH_ACQUIRED);

    memset(&fmt, 0, sizeof(mm_camera_ch_image_fmt_parm_t));
    fmt.ch_type = MM_CAMERA_CH_SNAPSHOT;
    fmt.snapshot.main.fmt = dim->main_img_format;
    fmt.snapshot.main.dim.width = dim->picture_width;
    fmt.snapshot.main.dim.height = dim->picture_height;

    fmt.snapshot.thumbnail.fmt = dim->thumb_format;
    fmt.snapshot.thumbnail.dim.width = dim->ui_thumbnail_width;
    fmt.snapshot.thumbnail.dim.height = dim->ui_thumbnail_height;

    LOGV("%s: Snapshot channel fmt = main: %d thumbnail: %d", __func__,
         dim->main_img_format, dim->thumb_format);
    LOGV("%s: Snapshot channel resolution = main: %dX%d  thumbnail: %dX%d",
         __func__, dim->picture_width, dim->picture_height,
         dim->ui_thumbnail_width, dim->ui_thumbnail_height);

    LOGD("%s: Set Snapshot channel image format", __func__);
    ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_CH_IMAGE_FMT, &fmt);
    if (NO_ERROR != ret) {
        LOGE("%s: Set Snapshot Channel format err=%d\n", __func__, ret);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    LOGD("%s: Register buffer notification. My object: %x",
         __func__, (unsigned int) this);
    (void) mmCamera->evt->register_buf_notify(mmCamera, MM_CAMERA_CH_SNAPSHOT, 
                                                snapshot_notify_cb, 
                                                this);
    /* Set the state to buffer notification completed */
    setSnapshotState(SNAPSHOT_STATE_BUF_NOTIF_REGD);

end:
    if (ret != NO_ERROR) {
        handleError();
    }
    LOGE("%s: X", __func__);
    return ret;

}

void QCameraStream_Snapshot::
deinitSnapshotChannel(mm_camera_channel_type_t ch_type)
{
    LOGD("%s: E", __func__);

    /* unreg buf notify*/
    if (getSnapshotState() >= SNAPSHOT_STATE_BUF_NOTIF_REGD){
        if (NO_ERROR != mmCamera->evt->register_buf_notify(
            mmCamera, ch_type, NULL, this)) {
            LOGE("%s: Failure to unregister buf notification", __func__);
        }
    }

    if (getSnapshotState() >= SNAPSHOT_STATE_CH_ACQUIRED) {
        LOGD("%s: Release snapshot channel", __func__);
        mmCamera->ops->ch_release(mmCamera, ch_type);
    }

    LOGD("%s: X",__func__);
}

status_t QCameraStream_Snapshot::
initRawSnapshotBuffers(cam_ctrl_dimension_t *dim, int num_of_buf)
{
    status_t ret = NO_ERROR;
    struct msm_frame *frame;
    uint32_t frame_len, y_off, cbcr_off;
    mm_camera_reg_buf_t reg_buf;
    
    LOGD("%s: E", __func__);

    if ((num_of_buf == 0) || (num_of_buf > MM_CAMERA_MAX_NUM_FRAMES)) {
        LOGE("%s: Invalid number of buffers (=%d) requested!", __func__, num_of_buf);
        ret = BAD_VALUE;
        goto end;
    }

    memset(&mSnapshotStreamBuf, 0, sizeof(mm_cameara_stream_buf_t));

    /* Number of buffers to be set*/
    mSnapshotStreamBuf.num = num_of_buf;

    /* Get a frame len for buffer to be allocated*/
    frame_len = mm_camera_get_msm_frame_len(CAMERA_BAYER_SBGGR10,
                                            myMode,
                                            dim->raw_picture_width,
                                            dim->raw_picture_height,
                                            &y_off, &cbcr_off,
                                            MM_CAMERA_PAD_WORD); 

    mSnapshotStreamBuf.frame_len = frame_len;

    /* Allocate Memory to store snapshot image */
    mRawSnapShotHeap =
        new PmemPool("/dev/pmem_adsp",
                     MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                     MSM_PMEM_RAW_MAINIMG,
                     frame_len,
                     num_of_buf,
                     frame_len,
                     cbcr_off,
                     y_off,
                     "snapshot camera");

    if (!mRawSnapShotHeap->initialized()) {
        mRawSnapShotHeap.clear();
        LOGE("%s: Error allocating buffer for raw snapshot", __func__);
        ret = NO_MEMORY;
        goto end;
    }

    for(int i = 0; i < mSnapshotStreamBuf.num; i++) {
        frame = &(mSnapshotStreamBuf.frame[i]);
        memset(frame, 0, sizeof(struct msm_frame));
        frame->fd = mRawSnapShotHeap->mHeap->getHeapID();
        frame->buffer = (uint32_t) mRawSnapShotHeap->mHeap->base() + 
            mRawSnapShotHeap->mAlignedBufferSize * i;
        frame->path = OUTPUT_TYPE_S;
        frame->cbcr_off = cbcr_off;
        frame->y_off = y_off;
    }/*end of for loop*/


    /* register the streaming buffers for the channel*/
    memset(&reg_buf,  0,  sizeof(mm_camera_reg_buf_t));
    reg_buf.ch_type = MM_CAMERA_CH_RAW;
    reg_buf.def.num = mSnapshotStreamBuf.num;
    reg_buf.def.frame = mSnapshotStreamBuf.frame;

    ret = mmCamera->cfg->prepare_buf(mmCamera, &reg_buf);
    if(ret != NO_ERROR) {
        LOGV("%s:reg snapshot buf err=%d\n", __func__, ret);
        mRawSnapShotHeap.clear();
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* If we have reached here successfully, we have allocated buffer.
       Set state machine.*/
    setSnapshotState(SNAPSHOT_STATE_BUF_INITIALIZED);

end:
    /* If it's error, we'll need to do some needful */
    if (ret != NO_ERROR) {
        handleError();
    }
    LOGD("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::deinitRawSnapshotBuffers(void)
{
    int ret = NO_ERROR;

    LOGD("%s: E", __func__);

    /* deinit buffers only if we have already allocated */
    if (getSnapshotState() >= SNAPSHOT_STATE_BUF_INITIALIZED ){

        LOGD("%s: Unpreparing Snapshot Buffer", __func__);
        ret = mmCamera->cfg->unprepare_buf(mmCamera, MM_CAMERA_CH_RAW);
        if(ret != NO_ERROR) {
            LOGE("%s:Unreg Raw snapshot buf err=%d\n", __func__, ret);
            ret = FAILED_TRANSACTION;
            goto end;
        }
    
        /* Clear raw heap*/
        if (mRawSnapShotHeap != NULL) {
            mRawSnapShotHeap.clear();
            mRawSnapShotHeap = NULL;
        }
    }
    
end:
    LOGD("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::
initSnapshotBuffers(cam_ctrl_dimension_t *dim, int num_of_buf)
{
    status_t ret = NO_ERROR;
    struct msm_frame *frame;
    uint32_t frame_len, y_off, cbcr_off;
    mm_camera_reg_buf_t reg_buf;
    uint32_t main_frame_offset[MM_CAMERA_MAX_NUM_FRAMES];
    uint32_t thumb_frame_offset[MM_CAMERA_MAX_NUM_FRAMES];
    
    LOGD("%s: E", __func__);

    if ((num_of_buf == 0) || (num_of_buf > MM_CAMERA_MAX_NUM_FRAMES)) {
        LOGE("%s: Invalid number of buffers (=%d) requested!",
             __func__, num_of_buf);
        ret = BAD_VALUE;
        goto end;
    }

    memset(&mSnapshotStreamBuf, 0, sizeof(mm_cameara_stream_buf_t));
    memset(&mPostviewStreamBuf, 0, sizeof(mm_cameara_stream_buf_t));
    /* Number of buffers to be set*/
    mSnapshotStreamBuf.num = num_of_buf;
    mPostviewStreamBuf.num = num_of_buf;

    /*TBD: to be modified for 3D*/
    mm_jpeg_encoder_get_buffer_offset( dim->picture_width, dim->picture_height,
                                       &y_off, &cbcr_off, &frame_len);
    mSnapshotStreamBuf.frame_len = frame_len;

    /* Allocate Memory to store snapshot image */
    mRawHeap =
        new PmemPool("/dev/pmem_adsp",
                     MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                     MSM_PMEM_MAINIMG,
                     frame_len,
                     num_of_buf,
                     frame_len,
                     cbcr_off,
                     y_off,
                     "snapshot camera");

    if (!mRawHeap->initialized()) {
        mRawHeap.clear();
        LOGE("%s: Error allocating buffer for snapshot", __func__);
        ret = NO_MEMORY;
        goto end;
    }

    for(int i = 0; i < mSnapshotStreamBuf.num; i++) {
        frame = &(mSnapshotStreamBuf.frame[i]);
        memset(frame, 0, sizeof(struct msm_frame));
        frame->fd = mRawHeap->mHeap->getHeapID();
        main_frame_offset[i] = mRawHeap->mAlignedBufferSize * i;
        frame->buffer = (uint32_t) mRawHeap->mHeap->base() + 
            main_frame_offset[i];
        frame->path = OUTPUT_TYPE_S;
        frame->cbcr_off = cbcr_off;
        frame->y_off = y_off;
    }/*end of for loop*/
   

    /* allocate memory for postview*/
    frame_len = mm_camera_get_msm_frame_len(dim->thumb_format, myMode,
                                            dim->ui_thumbnail_width,
                                            dim->ui_thumbnail_height,
                                            &y_off, &cbcr_off, MM_CAMERA_PAD_WORD);
    mPostviewStreamBuf.frame_len = frame_len;
      

     //Postview Image
     mPostviewHeap =
        new PmemPool("/dev/pmem_adsp",
                     MemoryHeapBase::READ_ONLY | MemoryHeapBase::NO_CACHING,
                     MSM_PMEM_THUMBNAIL,
                     frame_len,
                     num_of_buf,
                     frame_len,
                     cbcr_off,
                     y_off,
                     "thumbnail");

    if (!mPostviewHeap->initialized()) {
        mRawHeap.clear();
        mPostviewHeap.clear();
        LOGE("%s: Error initializing Postview buffer", __func__);
        ret = NO_MEMORY;
        goto end;
    }

    for(int i = 0; i < mPostviewStreamBuf.num; i++) {
        frame = &(mPostviewStreamBuf.frame[i]);
        memset(frame, 0, sizeof(struct msm_frame));
        frame->fd = mPostviewHeap->mHeap->getHeapID();
        thumb_frame_offset[i] = mPostviewHeap->mAlignedBufferSize * i;
        frame->buffer = (uint32_t)mPostviewHeap->mHeap->base() +
            thumb_frame_offset[i];
        frame->path = OUTPUT_TYPE_T;
        frame->cbcr_off = cbcr_off;
        frame->y_off = y_off;
    }/*end of for loop*/

    /* register the streaming buffers for the channel*/
    memset(&reg_buf,  0,  sizeof(mm_camera_reg_buf_t));
    reg_buf.ch_type = MM_CAMERA_CH_SNAPSHOT;
    reg_buf.snapshot.main.num = mSnapshotStreamBuf.num;
    reg_buf.snapshot.main.frame = mSnapshotStreamBuf.frame;
    reg_buf.snapshot.main.frame_offset = main_frame_offset;
    reg_buf.snapshot.thumbnail.num = mPostviewStreamBuf.num;
    reg_buf.snapshot.thumbnail.frame = mPostviewStreamBuf.frame;
    reg_buf.snapshot.thumbnail.frame_offset = thumb_frame_offset;

    ret = mmCamera->cfg->prepare_buf(mmCamera, &reg_buf);
    if(ret != NO_ERROR) {
        LOGV("%s:reg snapshot buf err=%d\n", __func__, ret);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* If we have reached here successfully, we have allocated buffer.
       Set state machine.*/
    setSnapshotState(SNAPSHOT_STATE_BUF_INITIALIZED);

end:
    if (ret != NO_ERROR) {
        handleError();
    }
    LOGD("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::
deinitSnapshotBuffers(void)
{
    int ret = NO_ERROR;

    LOGD("%s: E", __func__);

    /* Deinit only if have already initialized*/
    if (getSnapshotState() >= SNAPSHOT_STATE_BUF_INITIALIZED ){

        LOGD("%s: Unpreparing Snapshot Buffer", __func__);
        ret = mmCamera->cfg->unprepare_buf(mmCamera, MM_CAMERA_CH_SNAPSHOT);
        if(ret != NO_ERROR) {
            LOGE("%s:unreg snapshot buf err=%d\n", __func__, ret);
            ret = FAILED_TRANSACTION;
            goto end;
        }
    
        /* Clear main and thumbnail heap*/
        if (mRawHeap != NULL){
            mRawHeap.clear();
            mRawHeap = NULL;
        }
        if (mPostviewHeap != NULL) {
            mPostviewHeap.clear();
            mPostviewHeap = NULL;
        }
    
    }
end:
    LOGD("%s: X", __func__);
    return ret;
}

void QCameraStream_Snapshot::deinit(bool have_to_release)
{
    mm_camera_channel_type_t ch_type;

    LOGD("%s: E", __func__);
   
    /* If its ZSL mode and "have_to_release" flag isn't set,
       we don't need to deinit ZSL channel. We'll just return.*/
    if (isZSLMode() && have_to_release) {
        setSnapshotState(SNAPSHOT_STATE_INITIALIZED);
        return;
    }

    LOGD("%s: Number of snapshots remaining to process: %d",
         __func__, mNumOfSnapshot);

    /* In burst mode/multiple snapshot, we'll need to take multiple
       images. So before deinitializing we need to check whether we have
       already completed all snapshots. If yes then only we can deallocate
       the memory and release the channel.*/

    /* We should be able to deinit if we are explicitly told to release
       whatever state we are in. */
    if ((mNumOfSnapshot == 0) || have_to_release){
        if (mSnapshotFormat == PICTURE_FORMAT_RAW) {
            /* deinit buffer */
            deinitRawSnapshotBuffers();
            /* deinit channel */
            deinitSnapshotChannel(MM_CAMERA_CH_RAW);
        }
        else
        {
            deinitSnapshotBuffers();
            deinitSnapshotChannel(MM_CAMERA_CH_SNAPSHOT);
        }

        /* memset some global structure */
        memset(&mSnapshotStreamBuf, 0, sizeof(mSnapshotStreamBuf));
        memset(&mPostviewStreamBuf, 0, sizeof(mPostviewStreamBuf));
        mSnapshotQueue.flush();

        mNumOfSnapshot = 0;
        setSnapshotState(SNAPSHOT_STATE_UNINIT);
    }
    LOGD("%s: X", __func__);
}

/*Temp: Bikas: to be removed once event handling is enabled in mm-camera. 
  We need two events - one to call notifyShutter and other event for
  stream-off to disable OPS_SNAPSHOT*/
void QCameraStream_Snapshot::runSnapshotThread(void *data)
{
    LOGD("%s: E", __func__);
    /* play shutter sound */
    LOGD("%s:Play shutter sound only", __func__);

    notifyShutter(NULL, TRUE);

    /* TBD: Temp: Needs to be removed once event handling is enabled.
       We cannot call mm-camera interface to stop snapshot from callback
       function as it causes deadlock. Hence handling it here temporarily
       in this thread. Later mm-camera intf will give us event in separate
       thread context */
    mm_app_snapshot_wait();
    if (mSnapshotFormat == PICTURE_FORMAT_RAW) {
        /* Send command to stop snapshot polling thread*/
        stop();
    }
    LOGD("%s: X", __func__);
}

/*Temp: Bikas: to be removed once event handling is enabled in mm-camera*/
static void *snapshot_thread(void *obj)
{
    QCameraStream_Snapshot *pme = (QCameraStream_Snapshot *)obj;
    LOGD("%s: E", __func__);
    if (pme != 0) {
        pme->runSnapshotThread(obj);
    }
    else LOGW("not starting snapshot thread: the object went away!");
    LOGD("%s: X", __func__);
    return NULL;
}

/*Temp: Bikas: to be removed later*/
static pthread_t mSnapshotThread;

status_t QCameraStream_Snapshot::initSnapshot(int num_of_snapshots)
{
    status_t ret = NO_ERROR;
    cam_ctrl_dimension_t dim;
    mm_camera_op_mode_type_t op_mode;

    LOGV("%s: E", __func__);

    if (!mmCamera) {
        LOGE("%s: error - native camera is NULL!", __func__);
        LOGE("%s: X", __func__);
        ret = BAD_VALUE;
        goto end;
    }
    else{
        LOGD("%s: Get current dimension", __func__);
        /* Query mm_camera to get current dimension */
        memset(&dim, 0, sizeof(cam_ctrl_dimension_t));
        ret = mmCamera->cfg->get_parm(mmCamera,
                                      MM_CAMERA_PARM_DIMENSION,
                                      &dim);
        if (NO_ERROR != ret) {
            LOGE("%s: error - can't get preview dimension!", __func__);
            ret = FAILED_TRANSACTION;
            goto end;
        }
    }

    /* Set camera op mode to MM_CAMERA_OP_MODE_CAPTURE */
    LOGD("Setting OP_MODE_CAPTURE");
    op_mode = MM_CAMERA_OP_MODE_CAPTURE;
    if( NO_ERROR != mmCamera->cfg->set_parm(
            mmCamera, MM_CAMERA_PARM_OP_MODE, &op_mode)) {
        LOGE("%s: MM_CAMERA_OP_MODE_CAPTURE failed", __func__);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* config the parmeters and see if we need to re-init the stream*/
    LOGD("%s: Configure Snapshot Dimension", __func__);
    ret = configSnapshotDimension(&dim);
    if (ret != NO_ERROR) {
        LOGE("%s: Setting snapshot dimension failed", __func__);
        goto end;
    }

    /* Initialize stream - set format, acquire channel */
    ret = initSnapshotChannel(&dim);
    if (NO_ERROR != ret) {
        LOGE("%s: error - can't init nonZSL stream!", __func__);
        goto end;
    }

    ret = initSnapshotBuffers(&dim, num_of_snapshots);
    if ( NO_ERROR != ret ){
        LOGE("%s: Failure allocating memory for Snapshot buffers", __func__);
        goto end;
    }

end:
    /* Based on what state we are in, we'll need to handle error -
       like deallocating memory if we have already allocated */
    if (ret != NO_ERROR) {
        handleError();
    }
	LOGV("%s: X", __func__);
    return ret;

}

status_t QCameraStream_Snapshot::initRawSnapshot(int num_of_snapshots)
{
    status_t ret = NO_ERROR;
    cam_ctrl_dimension_t dim;
    bool initSnapshot = false;
    mm_camera_op_mode_type_t op_mode;
    mm_camera_reg_buf_t reg_buf;
    mm_camera_raw_streaming_type_t raw_stream_type = 
        MM_CAMERA_RAW_STREAMING_CAPTURE_SINGLE;

    LOGV("%s: E", __func__);

    if (!mmCamera) {
        LOGE("%s: error - native camera is NULL!", __func__);
        LOGE("%s: X", __func__);
        ret = BAD_VALUE;
        goto end;
    }

    /* Set camera op mode to MM_CAMERA_OP_MODE_CAPTURE */
    LOGD("%s: Setting OP_MODE_CAPTURE", __func__);
    op_mode = MM_CAMERA_OP_MODE_CAPTURE;
    if( NO_ERROR != mmCamera->cfg->set_parm(
            mmCamera, MM_CAMERA_PARM_OP_MODE, &op_mode)) {
        LOGE("%s: MM_CAMERA_OP_MODE_CAPTURE failed", __func__);
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* For raw snapshot, we do not know the dimension as it
       depends on sensor to sensor. We call setDimension which will
       give us raw width and height */
    LOGD("%s: Get Raw Snapshot Dimension", __func__);
    ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_DIMENSION,&dim);
    if (NO_ERROR != ret) {
      LOGE("%s: error - can't set snapshot parms!", __func__);
      ret = FAILED_TRANSACTION;
      goto end;
    }
    LOGD("%s: Raw Snapshot dimension: %dx%d", __func__, 
         dim.raw_picture_width,
         dim.raw_picture_height);

    /* Initialize stream - set format, acquire channel */
    /*TBD: Currently we only support single raw capture*/
    if (num_of_snapshots == 1) {
        raw_stream_type = MM_CAMERA_RAW_STREAMING_CAPTURE_SINGLE;
    }

    ret = initRawSnapshotChannel(&dim, raw_stream_type);
    if (NO_ERROR != ret) {
        LOGE("%s: error - can't init nonZSL stream!", __func__);
        goto end;
    }

    ret = initRawSnapshotBuffers(&dim, num_of_snapshots);
    if ( NO_ERROR != ret ){
        LOGE("%s: Failure allocating memory for Raw Snapshot buffers",
             __func__);
        goto end;
    }

end:
    if (ret != NO_ERROR) {
        handleError();
    }
	LOGV("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::initZSLSnapshot(void)
{
    status_t ret = NO_ERROR;
    cam_ctrl_dimension_t dim;
    bool initSnapshot = false;
    mm_camera_op_mode_type_t op_mode;

    LOGV("%s: E", __func__);

    if (!mmCamera) {
        LOGE("%s: error - native camera is NULL!", __func__);
        LOGE("%s: X", __func__);
        ret = BAD_VALUE;
        goto end;
    }
    else{
        LOGD("%s: Get current dimension", __func__);
        /* Query mm_camera to get current dimension */
        memset(&dim, 0, sizeof(cam_ctrl_dimension_t));
        ret = mmCamera->cfg->get_parm(mmCamera,
                                      MM_CAMERA_PARM_DIMENSION,
                                      &dim);
        if (NO_ERROR != ret) {
            LOGE("%s: error - can't get preview dimension!", __func__);
            ret = FAILED_TRANSACTION;
            goto end;
        }
    }

    /* Set camera op mode to MM_CAMERA_OP_MODE_ZSL */
    LOGD("Setting OP_MODE_ZSL");
    op_mode = MM_CAMERA_OP_MODE_ZSL;
    if( NO_ERROR != mmCamera->cfg->set_parm(
            mmCamera, MM_CAMERA_PARM_OP_MODE, &op_mode)) {
        LOGE("SET MODE: MM_CAMERA_OP_MODE_ZSL failed");
        ret = FAILED_TRANSACTION;
        goto end;
    }

    /* config the parmeters and see if we need to re-init the stream*/
    LOGD("%s: Configure Snapshot Dimension", __func__);
    initSnapshot = configSnapshotDimension(&dim);
    if (!initSnapshot) {
        ret = mmCamera->cfg->set_parm(mmCamera, MM_CAMERA_PARM_DIMENSION,&dim);
        if (NO_ERROR != ret) {
          LOGE("%s: error - can't config snapshot parms!", __func__);
          ret = FAILED_TRANSACTION;
          goto end;
        }
    }

    /* Initialize stream - set format, acquire channel */
    ret = initSnapshotChannel(&dim);
    if (NO_ERROR != ret) {
        LOGE("%s: error - can't init nonZSL stream!", __func__);
        goto end;
    }

    ret = initSnapshotBuffers(&dim, ZSL_BUFFER_NUM);
    if ( NO_ERROR != ret ){
        LOGE("%s: Failure allocating memory for Snapshot buffers", __func__);
        goto end;
    }

    /* Start ZSL - it'll start queuing the frames */
    LOGD("%s: Call MM_CAMERA_OPS_SNAPSHOT", __func__);
    if (NO_ERROR != mmCamera->ops->action(mmCamera,
                                          TRUE,
                                          MM_CAMERA_OPS_ZSL,
                                          this)) {
           LOGE("%s: Failure taking snapshot", __func__);
           ret = FAILED_TRANSACTION;
           goto end;
    }

end:
    /* Based on what state we are in, we'll need to handle error -
       like deallocating memory if we have already allocated */
    if (ret != NO_ERROR) {
        handleError();
    }
	LOGV("%s: X", __func__);
    return ret;

}

status_t QCameraStream_Snapshot::
takePictureJPEG(void)
{
    status_t ret = NO_ERROR;

    LOGD("%s: E", __func__);


    /* Take snapshot */
    LOGD("%s: Call MM_CAMERA_OPS_SNAPSHOT", __func__);
    if (NO_ERROR != mmCamera->ops->action(mmCamera,
                                              TRUE,
                                              MM_CAMERA_OPS_SNAPSHOT,
                                              this)) {
           LOGE("%s: Failure taking snapshot", __func__);
           ret = FAILED_TRANSACTION;
           goto end;
    }

    /* TBD: Bikas: Temp: to be removed once event callback
       is implemented in mm-camera lib  */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&mSnapshotThread,&attr,
                   snapshot_thread, (void *)this);

end:
    if (ret != NO_ERROR) {
        handleError();
    }

	LOGD("%s: X", __func__);
    return ret;

}

status_t QCameraStream_Snapshot::
takePictureRaw(void)
{
    status_t ret = NO_ERROR;

    LOGD("%s: E", __func__);

    /* Take snapshot */
    LOGD("%s: Call MM_CAMERA_OPS_SNAPSHOT", __func__);
    if (NO_ERROR != mmCamera->ops->action(mmCamera,
                                              TRUE,
                                              MM_CAMERA_OPS_RAW,
                                              this)) {
           LOGE("%s: Failure taking snapshot", __func__);
           ret = FAILED_TRANSACTION;
           goto end;
    }

    /* TBD: Bikas: Temp: to be removed once event callback
       is implemented in mm-camera lib  */
    /* Wait for snapshot frame callback to return*/
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&mSnapshotThread,&attr,
                   snapshot_thread, (void *)this);

end:
    if (ret != NO_ERROR) {
        handleError();
    }
	LOGD("%s: X", __func__);
    return ret;

}

status_t QCameraStream_Snapshot::
takePictureZSL(void)
{
    status_t ret = NO_ERROR;

    LOGD("%s: E", __func__);


	LOGD("%s: X", __func__);
    return ret;

}

status_t  QCameraStream_Snapshot::
encodeData(mm_camera_ch_data_buf_t* recvd_frame)
{
    status_t ret = NO_ERROR;
    cam_ctrl_dimension_t dimension;
    struct msm_frame *postviewframe;
    struct msm_frame *mainframe;
    common_crop_t crop_info;

    LOGV("%s: E", __func__);

    postviewframe = recvd_frame->snapshot.thumbnail.frame;
    mainframe = recvd_frame->snapshot.main.frame;

    dimension.orig_picture_dx = mPictureWidth;
    dimension.orig_picture_dy = mPictureHeight;
    dimension.thumbnail_width = mThumbnailWidth;
    dimension.thumbnail_height = mThumbnailHeight;

    LOGD("%s:Allocating memory to store jpeg image."
         "main image size: %dX%d frame_len: %d", __func__,
         mPictureWidth, mPictureHeight, mSnapshotStreamBuf.frame_len);
    mJpegHeap = new AshmemPool(mSnapshotStreamBuf.frame_len,
                               1,
                               0, // we do not know how big the picture will be
                               "jpeg");
    if (!mJpegHeap->initialized()) {
        mJpegHeap.clear();
        LOGE("%s: Error allocating JPEG memory", __func__);
        ret = NO_MEMORY;
        goto end;
    }
  
    /*TBD: Bikas: Move JPEG handling to the mm-camera library */
    LOGD("Setting callbacks, initializing encoder and start encoding.");
    LOGD(" Passing my obj: %x", (unsigned int) this);
    set_callbacks(snapshot_jpeg_fragment_cb, snapshot_jpeg_cb, this);
    mm_jpeg_encoder_init();
    mm_jpeg_encoder_setMainImageQuality(mHalCamCtrl->getJpegQuality());
    LOGD("Dimension to encode: main: %dx%d thumbnail: %dx%d",
         dimension.orig_picture_dx, dimension.orig_picture_dy,
         dimension.thumbnail_width, dimension.thumbnail_height);

    /* Logic here:
	If it's the only frame, we directly pass to encoder.
	If not, we'll queue it and check during next jpeg callback*/
    if(getSnapshotState() == SNAPSHOT_STATE_JPEG_ENCODING) {
        /* encoding is going on. Just queue the frame for now.*/
        mSnapshotQueue.enqueue((void *)recvd_frame);
        
    }
    else{
         /*TBD: Pass 0 as cropinfo for now as v4l2 doesn't provide 
           cropinfo. It'll be changed later.*/
         memset(&crop_info,0,sizeof(common_crop_t));
         mm_jpeg_encoder_encode((const cam_ctrl_dimension_t *)&dimension,
                           (uint8_t *)postviewframe->buffer,
                           postviewframe->fd,
                           (uint8_t *)mainframe->buffer,
                           mainframe->fd,
                           &crop_info,
                           NULL,
                           0,
                           -1,
                           NULL,
                           NULL);
 
        /* Save the pointer to the frame sent for encoding. we'll need it to
           tell kernel that we are done with the frame.*/
        mCurrentFrameEncoded = recvd_frame;
    }

end:
    if (ret == NO_ERROR) {
        setSnapshotState(SNAPSHOT_STATE_JPEG_ENCODING);
    }
	LOGV("%s: X", __func__);
    return ret;
}

/* Called twice - 1st to play shutter sound and 2nd to configure
   overlay/surfaceflinger for postview */
void QCameraStream_Snapshot::notifyShutter(common_crop_t *crop,
                                           bool mPlayShutterSoundOnly)
{
    image_rect_type size;
    LOGD("%s: E", __func__);

    if(mPlayShutterSoundOnly) {
        /* At this point, invoke Notify Callback to play shutter sound only.
         * We want to call notify callback again when we have the
         * yuv picture ready. This is to reduce blanking at the time
         * of displaying postview frame. Using ext2 to indicate whether
         * to play shutter sound only or register the postview buffers.
         */
        LOGD("%s: Calling callback to play shutter sound", __func__);
        mHalCamCtrl->mNotifyCb(CAMERA_MSG_SHUTTER, 0, 
                                     mPlayShutterSoundOnly,
                                     mHalCamCtrl->mCallbackCookie);
        return;
    }

    if (mHalCamCtrl->mNotifyCb && 
        (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_SHUTTER)) {
        /* TBD:Bikas: To be checked later.itz giving page fault */
        //mDisplayHeap = mPostviewHeap;
        if (crop != NULL && (crop->in1_w != 0 && crop->in1_h != 0)) {
            size.width = crop->in1_w;
            size.height = crop->in1_h;
            LOGD("%s: Size from cropinfo: %dX%d", __func__,
                 size.width, size.height);
        }
        else {
            size.width = mPostviewWidth;
            size.height = mPostviewHeight;
            LOGD("%s: Size from global: %dX%d", __func__,
                 size.width, size.height);
        }
        /*if(strTexturesOn == true) {
            mDisplayHeap = mRawHeap;
            size.width = mPictureWidth;
            size.height = mPictureHeight;
        }*/
        /* Now, invoke Notify Callback to unregister preview buffer
         * and register postview buffer with surface flinger. Set ext2
         * as 0 to indicate not to play shutter sound.
         */

        mHalCamCtrl->mNotifyCb(CAMERA_MSG_SHUTTER, (int32_t)&size, 0,
                                     mHalCamCtrl->mCallbackCookie);
    }
    LOGD("%s: X", __func__);
}

status_t  QCameraStream_Snapshot::
encodeDisplayAndSave(mm_camera_ch_data_buf_t* recvd_frame)
{
    status_t ret = NO_ERROR;
    struct msm_frame *postview_frame;
    common_crop_t *crop;
    common_crop_t *cropp;
    int buf_index = 0;
    ssize_t offset_addr = 0;

    /* send frame for encoding */
    LOGD("%s: Send frame for encoding", __func__);
    ret = encodeData(recvd_frame);
    if (ret != NO_ERROR) {
        LOGE("%s: Failure configuring JPEG encoder", __func__);

        /* Failure encoding this frame. Just notify upper layer
           about it.*/
        if(mHalCamCtrl->mDataCb &&
            (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)) {
            /* get picture failed. Give jpeg callback with NULL data
             * to the application to restore to preview mode
             */
            mHalCamCtrl->mDataCb(CAMERA_MSG_COMPRESSED_IMAGE,
                                       NULL,
                                       mHalCamCtrl->mCallbackCookie);
        }
        goto end;
    }

    /* Display postview image*/
    postview_frame = recvd_frame->snapshot.thumbnail.frame;
    /* TBD: currently we don't get cropinfo. So setting it to NULL*/
    cropp = NULL;
    LOGD("%s: Displaying Postview Image", __func__);
    offset_addr = (ssize_t)postview_frame->buffer -
        (ssize_t)mPostviewHeap->mHeap->base();
    if(mHalCamCtrl->mUseOverlay) {
        mHalCamCtrl->mOverlayLock.lock();
        if(mHalCamCtrl->mOverlay != NULL) {
            mHalCamCtrl->mOverlay->setFd(postview_frame->fd);

            if(cropp != NULL){
                crop = (common_crop_t *)cropp;
                if (crop->in1_w != 0 && crop->in1_h != 0) {
                    int x = (crop->out1_w - crop->in1_w + 1) / 2 - 1;
                    int y = (crop->out1_h - crop->in1_h + 1) / 2 - 1;
                    int w = crop->in1_w;
                    int h = crop->in1_h;
                    if(x < 0) x = 0;
                    if(y < 0) y = 0;
                    mHalCamCtrl->mOverlay->setCrop(x, y,w,h);
                }else {
                    mHalCamCtrl->mOverlay->setCrop(0, 0,
                                                   mPostviewWidth,
                                                   mPostviewHeight);
                }
            }
            LOGV(" Queueing Postview for display ");
            mHalCamCtrl->mOverlay->queueBuffer((void *)offset_addr);
        }
        mHalCamCtrl->mOverlayLock.unlock();
    }
     
    // send upperlayer callback
     if (mHalCamCtrl->mDataCb && 
         (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_RAW_IMAGE)){
         buf_index = recvd_frame->snapshot.main.idx;
         mHalCamCtrl->mDataCb(CAMERA_MSG_RAW_IMAGE, 
                                    mRawHeap->mBuffers[buf_index],
                                    mHalCamCtrl->mCallbackCookie);
     }

end:
    LOGD("%s: X", __func__);
    return ret;
}


void QCameraStream_Snapshot::receiveRawPicture(mm_camera_ch_data_buf_t* recvd_frame)
{
    int buf_index = 0;
    common_crop_t crop;

    LOGD("%s: E", __func__);
/*
    mm_app_dump_snapshot_frame(recvd_frame->snapshot.main.frame, 
                               mSnapshotStreamBuf.frame_len, 
                               TRUE, 0);
    mm_app_dump_snapshot_frame(recvd_frame->snapshot.thumbnail.frame,
                               mPostviewStreamBuf.frame_len, 
                               FALSE, 1);
*/

    /* If it's raw snapshot, we just want to tell upperlayer to save the image*/
    if(mSnapshotFormat == PICTURE_FORMAT_RAW) {
        LOGD("%s: Call notifyShutter 2nd time", __func__);
        notifyShutter(NULL, FALSE);
        LOGD("%s: Sending Raw Snapshot Callback to Upperlayer", __func__);
        buf_index = recvd_frame->def.idx;
        if (mHalCamCtrl->mDataCb && 
            (mHalCamCtrl->mMsgEnabled & CAMERA_MSG_COMPRESSED_IMAGE)){
                mHalCamCtrl->mDataCb(
                    CAMERA_MSG_COMPRESSED_IMAGE,
                    mRawSnapShotHeap->mBuffers[buf_index],
                    mHalCamCtrl->mCallbackCookie);
        }

        /* TBD: Temp: To be removed once event handling is enabled */
        mm_app_snapshot_done();
    }
    else{
        /*TBD: v4l2 doesn't have support to provide cropinfo along with 
          frame. We'll need to query.*/
        memset(&crop, 0, sizeof(common_crop_t));
        LOGD("%s: Call notifyShutter 2nd time", __func__);
        notifyShutter(&crop, FALSE);

        if ( NO_ERROR != encodeDisplayAndSave(recvd_frame)){
            LOGE("%s: Error while encoding/displaying/saving image", __func__);
        }
    }

    /* One more frame has been sent/queued for encoding. Update number of
       snapshots remaining (for burst/continuous mode) */
    mNumOfSnapshot--;
    
    LOGD("%s: X", __func__);
}

//-------------------------------------------------------------------
// Helper Functions
//-------------------------------------------------------------------
void QCameraStream_Snapshot::handleError()
{
    mm_camera_channel_type_t ch_type;
    LOGD("%s: E", __func__);

    /* Depending upon the state we'll have to
       handle error */
    switch(getSnapshotState()) {
    case SNAPSHOT_STATE_JPEG_ENCODING:
        if(mJpegHeap != NULL) mJpegHeap.clear();
        mJpegHeap = NULL;

    case SNAPSHOT_STATE_YUV_RECVD:
    case SNAPSHOT_STATE_IMAGE_CAPTURE_STRTD:
    case SNAPSHOT_STATE_INITIALIZED:
    case SNAPSHOT_STATE_BUF_INITIALIZED:
        if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
            deinitSnapshotBuffers();
        }else
        {
            deinitRawSnapshotBuffers();
        }
    case SNAPSHOT_STATE_BUF_NOTIF_REGD:
    case SNAPSHOT_STATE_CH_ACQUIRED:
        if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
            deinitSnapshotChannel(MM_CAMERA_CH_SNAPSHOT);
        }else
        {
            deinitSnapshotChannel(MM_CAMERA_CH_RAW);
        }
    default:
        /* Set the state to ERROR */
        setSnapshotState(SNAPSHOT_STATE_ERROR);
        break;
    }
    
    LOGD("%s: X", __func__);
}

void QCameraStream_Snapshot::setSnapshotState(int state)
{
    LOGD("%s: Setting snapshot state to: %d",
         __func__, state);
    mSnapshotState = state;
}

int QCameraStream_Snapshot::getSnapshotState()
{
    return mSnapshotState;
}

bool QCameraStream_Snapshot::isZSLMode()
{
    return (myMode == CAMERA_ZSL_MODE) ? true : false;
}

//------------------------------------------------------------------
// Constructor and Destructor
//------------------------------------------------------------------
QCameraStream_Snapshot::
QCameraStream_Snapshot(mm_camera_t *native_camera, camera_mode_t mode)
  : mmCamera(native_camera),
    myMode (mode),
    mJpegOffset(0),
    mSnapshotFormat(PICTURE_FORMAT_JPEG),
    mPictureWidth(0), mPictureHeight(0),
    mThumbnailWidth(0), mThumbnailHeight(0),
    mPostviewWidth(0), mPostviewHeight(0),
    mSnapshotState(SNAPSHOT_STATE_UNINIT),
    mJpegHeap(NULL), mRawHeap(NULL), mDisplayHeap(NULL),
    mPostviewHeap(NULL), mRawSnapShotHeap(NULL),
    mNumOfSnapshot(0), mCurrentFrameEncoded(NULL)
  {
    LOGV("%s: E", __func__);

    /*initialize snapshot queue*/
    mSnapshotQueue.init();

    LOGV("%s: X", __func__);
  }


QCameraStream_Snapshot::~QCameraStream_Snapshot() {
    LOGV("%s: E", __func__);

    /* deinit snapshot queue */
    if (mSnapshotQueue.isInitialized()) {
        mSnapshotQueue.deinit();
    }

    LOGV("%s: X", __func__);

}

//------------------------------------------------------------------
// Public Members
//------------------------------------------------------------------
status_t QCameraStream_Snapshot::init() 
{
    status_t ret = NO_ERROR;

    LOGV("%s: E", __func__);

    /* Check the state. If we have already started snapshot
       process just return*/
    if (getSnapshotState() != SNAPSHOT_STATE_UNINIT) {
        ret = INVALID_OPERATION;
        LOGE("%s: Trying to take picture while snapshot is in progress",
             __func__);
        goto end;
    }

    /* Keep track of number of snapshots to take - in case of
       multiple snapshot/burst mode */
    /* TBD: keeping number of snapshot as 1 for now*/
    //mNumOfSnapshot = num_of_snapshots;
    mNumOfSnapshot = 1;

    /* Check if it's a ZSL mode */
    if (isZSLMode()) {
        ret = initZSLSnapshot();
        goto end;
    }
    /* Check if it's a raw snapshot or JPEG*/
    if( mHalCamCtrl->isRawSnapshot()) {
        mSnapshotFormat = PICTURE_FORMAT_RAW;
        ret = initRawSnapshot(mNumOfSnapshot);
        goto end;
    }
    else{
        mSnapshotFormat = PICTURE_FORMAT_JPEG;
        ret = initSnapshot(mNumOfSnapshot);
        goto end;
    }

end:
    if (ret == NO_ERROR) {
        setSnapshotState(SNAPSHOT_STATE_INITIALIZED);
    }
    LOGV("%s: X", __func__);
    return ret;
}

status_t QCameraStream_Snapshot::start() {
    status_t ret = NO_ERROR;

    LOGV("%s: E", __func__);
    if (isZSLMode()) {
        ret = takePictureZSL();
        goto end;
    }
    if (mSnapshotFormat == PICTURE_FORMAT_RAW) {
        ret = takePictureRaw();
        goto end;
    }
    else{
        ret = takePictureJPEG();
        goto end;
    }

end:
    if (ret == NO_ERROR) {
        setSnapshotState(SNAPSHOT_STATE_IMAGE_CAPTURE_STRTD);
    }
    LOGV("%s: X", __func__);
    return ret;
  }

void QCameraStream_Snapshot::stop(void)
{
    mm_camera_ops_type_t ops_type;

    LOGV("%s: E", __func__);
    if (getSnapshotState() != SNAPSHOT_STATE_UNINIT) {
        if (mSnapshotFormat == PICTURE_FORMAT_JPEG) {
            ops_type = isZSLMode() ? MM_CAMERA_OPS_ZSL : MM_CAMERA_OPS_SNAPSHOT;
        }
        else
           ops_type = MM_CAMERA_OPS_RAW;
    
        if( NO_ERROR != mmCamera->ops->action(
                        mmCamera, FALSE, 
                        ops_type, this)) {
            LOGE("%s: Failure stopping snapshot", __func__);
        }
    
        /* Depending upon current state, we'll need to allocate-deallocate-deinit*/
        deinit(1);
    }
    LOGV("%s: X", __func__);

}

void QCameraStream_Snapshot::release()
{
    LOGV("%s: E", __func__);

    /* release is generally called in case of explicit call from
       upper-layer during disconnect. So we need to deinit everything
       whatever state we are in */
    deinit(1);

    LOGV("%s: X", __func__);

}

void QCameraStream_Snapshot::prepareHardware()
{
    LOGV("%s: E", __func__);

    /* Prepare snapshot*/
    mmCamera->ops->action(mmCamera, 
                          TRUE,
                          MM_CAMERA_OPS_PREPARE_SNAPSHOT,
                          this);
    LOGV("%s: X", __func__);
}

sp<IMemoryHeap> QCameraStream_Snapshot::getRawHeap() const
{
    return ((mDisplayHeap != NULL) ? mDisplayHeap->mHeap : NULL);
}

QCameraStream* 
QCameraStream_Snapshot::createInstance(mm_camera_t *native_camera,
                                      camera_mode_t mode)
{

  QCameraStream* pme = new QCameraStream_Snapshot(native_camera, mode);
  
  return pme;
}

void QCameraStream_Snapshot::deleteInstance(QCameraStream *p)
{
  if (p){
    p->release();
    delete p;
  }
}

}; // namespace android
