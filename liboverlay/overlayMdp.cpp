/*
* Copyright (C) 2008 The Android Open Source Project
* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "overlayUtils.h"
#include "overlayMdp.h"

#undef ALOG_TAG
#define ALOG_TAG "overlay"

namespace ovutils = overlay::utils;
namespace overlay {
bool MdpCtrl::init(uint32_t fbnum) {
    // FD init
    if(!utils::openDev(mFd, fbnum,
                Res::fbPath, O_RDWR)){
        ALOGE("Ctrl failed to init fbnum=%d", fbnum);
        return false;
    }
    return true;
}

void MdpCtrl::reset() {
    utils::memset0(mOVInfo);
    utils::memset0(mLkgo);
    mOVInfo.id = MSMFB_NEW_REQUEST;
    mLkgo.id = MSMFB_NEW_REQUEST;
}

bool MdpCtrl::close() {
    if(MSMFB_NEW_REQUEST == static_cast<int>(mOVInfo.id))
        return true;
    if(!mdp_wrapper::unsetOverlay(mFd.getFD(), mOVInfo.id)) {
        ALOGE("MdpCtrl close error in unset");
        return false;
    }
    reset();
    if(!mFd.close()) {
        return false;
    }
    return true;
}

bool MdpCtrl::getScreenInfo(overlay::utils::ScreenInfo& info) {
    fb_fix_screeninfo finfo;
    if (!mdp_wrapper::getFScreenInfo(mFd.getFD(), finfo)) {
        return false;
    }

    fb_var_screeninfo vinfo;
    if (!mdp_wrapper::getVScreenInfo(mFd.getFD(), vinfo)) {
        return false;
    }
    info.mFBWidth   = vinfo.xres;
    info.mFBHeight  = vinfo.yres;
    info.mFBbpp     = vinfo.bits_per_pixel;
    info.mFBystride = finfo.line_length;
    return true;
}

bool MdpCtrl::get() {
    mdp_overlay ov;
    ov.id = mOVInfo.id;
    if (!mdp_wrapper::getOverlay(mFd.getFD(), ov)) {
        ALOGE("MdpCtrl get failed");
        return false;
    }
    mOVInfo = ov;
    return true;
}

//Adjust width, height, format if rotator is used.
void MdpCtrl::adjustSrcWhf(const bool& rotUsed) {
    if(rotUsed) {
        utils::Whf whf = getSrcWhf();
        if(whf.format == MDP_Y_CRCB_H2V2_TILE ||
                whf.format == MDP_Y_CBCR_H2V2_TILE) {
            whf.w = utils::alignup(whf.w, 64);
            whf.h = utils::alignup(whf.h, 32);
        }
        //For example: If original format is tiled, rotator outputs non-tiled,
        //so update mdp's src fmt to that.
        whf.format = utils::getRotOutFmt(whf.format);
        setSrcWhf(whf);
    }
}

bool MdpCtrl::set() {
    if(!mdp_wrapper::setOverlay(mFd.getFD(), mOVInfo)) {
        ALOGE("MdpCtrl failed to setOverlay, restoring last known "
                "good ov info");
        mdp_wrapper::dump("== Bad OVInfo is: ", mOVInfo);
        mdp_wrapper::dump("== Last good known OVInfo is: ", mLkgo);
        this->restore();
        return false;
    }
    this->save();
    return true;
}

bool MdpCtrl::setSource(const utils::PipeArgs& args) {

    setSrcWhf(args.whf);

    //TODO These are hardcoded. Can be moved out of setSource.
    mOVInfo.alpha = 0xff;
    mOVInfo.transp_mask = 0xffffffff;

    //TODO These calls should ideally be a part of setPipeParams API
    setFlags(args.mdpFlags);
    setZ(args.zorder);
    setWait(args.wait);
    setIsFg(args.isFg);
    return true;
}

bool MdpCtrl::setCrop(const utils::Dim& d) {
    setSrcRectDim(d);
    return true;
}

bool MdpCtrl::setTransform(const utils::eTransform& orient,
        const bool& rotUsed) {

    int rot = utils::getMdpOrient(orient);
    setUserData(rot);
    adjustSrcWhf(rotUsed);
    setRotationFlags();

    switch(static_cast<int>(orient)) {
        case utils::OVERLAY_TRANSFORM_0:
        case utils::OVERLAY_TRANSFORM_FLIP_H:
        case utils::OVERLAY_TRANSFORM_FLIP_V:
        case utils::OVERLAY_TRANSFORM_ROT_180:
            //No calculations required
            break;
        case utils::OVERLAY_TRANSFORM_ROT_90:
        case (utils::OVERLAY_TRANSFORM_ROT_90|utils::OVERLAY_TRANSFORM_FLIP_H):
        case (utils::OVERLAY_TRANSFORM_ROT_90|utils::OVERLAY_TRANSFORM_FLIP_V):
            overlayTransFlipRot90();
            break;
        case utils::OVERLAY_TRANSFORM_ROT_270:
            overlayTransFlipRot270();
            break;
        default:
            ALOGE("%s: Error due to unknown rot value", __FUNCTION__);
            return false;
    }
    return true;
}

void MdpCtrl::overlayTransFlipRot90()
{
    utils::Dim d   = getSrcRectDim();
    utils::Whf whf = getSrcWhf();
    int tmp = d.x;
    d.x = compute(whf.h,
            d.y,
            d.h);
    d.y = tmp;
    setSrcRectDim(d);
    swapSrcWH();
    swapSrcRectWH();
}

void MdpCtrl::overlayTransFlipRot270()
{
    utils::Dim d   = getSrcRectDim();
    utils::Whf whf = getSrcWhf();
    int tmp = d.y;
    d.y = compute(whf.w,
            d.x,
            d.w);
    d.x = tmp;
    setSrcRectDim(d);
    swapSrcWH();
    swapSrcRectWH();
}

bool MdpCtrl::setPosition(const overlay::utils::Dim& d,
        int fbw, int fbh)
{
    // Validatee against FB size
    if(!d.check(fbw, fbh)) {
        ALOGE("MdpCtrl setPosition failed dest dim violate screen limits");
        return false;
    }

    ovutils::Dim dim(d);
    ovutils::Dim ovsrcdim = getSrcRectDim();
    // Scaling of upto a max of 20 times supported
    if(dim.w >(ovsrcdim.w * ovutils::HW_OV_MAGNIFICATION_LIMIT)){
        dim.w = ovutils::HW_OV_MAGNIFICATION_LIMIT * ovsrcdim.w;
        dim.x = (fbw - dim.w) / 2;
    }
    if(dim.h >(ovsrcdim.h * ovutils::HW_OV_MAGNIFICATION_LIMIT)) {
        dim.h = ovutils::HW_OV_MAGNIFICATION_LIMIT * ovsrcdim.h;
        dim.y = (fbh - dim.h) / 2;
    }

    setDstRectDim(dim);
    return true;
}

void MdpCtrl::dump() const {
    ALOGE("== Dump MdpCtrl start ==");
    mFd.dump();
    mdp_wrapper::dump("mOVInfo", mOVInfo);
    ALOGE("== Dump MdpCtrl end ==");
}

void MdpData::dump() const {
    ALOGE("== Dump MdpData start ==");
    mFd.dump();
    mdp_wrapper::dump("mOvData", mOvData);
    ALOGE("== Dump MdpData end ==");
}

void MdpCtrl3D::dump() const {
    ALOGE("== Dump MdpCtrl start ==");
    mFd.dump();
    ALOGE("== Dump MdpCtrl end ==");
}

} // overlay