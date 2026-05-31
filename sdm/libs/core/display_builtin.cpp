/*
* Copyright (c) 2014 - 2021, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted
* provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright notice, this list of
*      conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright notice, this list of
*      conditions and the following disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written
*      permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Changes from Qualcomm Technologies, Inc. are provided under the following license:
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
*/

#include "display_builtin.h"

#include <core/buffer_allocator.h>
#include <display_properties.h>
#include <private/aiqe_ssrc_feature_factory.h>
#include <private/hw_info_interface.h>
#include <private/hw_interface.h>
#include <sys/mman.h>
#include <utils/constants.h>
#include <utils/debug.h>
#include <utils/formats.h>
#include <utils/rect.h>
#include <utils/utils.h>

#include <algorithm>
#include <functional>
#include <iomanip>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "drm_interface.h"
#include "drm_master.h"

#define __CLASS__ "DisplayBuiltIn"

namespace sdm {

DisplayBuiltIn::DisplayBuiltIn(DisplayEventHandler *event_handler,
                               sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                               BufferAllocator *buffer_allocator, CompManager *comp_manager,
                               std::shared_ptr<IPCIntf> ipc_intf)
    : DisplayBase(kBuiltIn, event_handler, kDeviceBuiltIn, buffer_allocator, comp_manager,
                  hw_info_intf),
      ipc_intf_(ipc_intf) {}

DisplayBuiltIn::DisplayBuiltIn(DisplayId display_id, DisplayEventHandler *event_handler,
                               sdm::MultiCoreInstance<uint32_t, HWInfoInterface *> hw_info_intf,
                               BufferAllocator *buffer_allocator, CompManager *comp_manager,
                               std::shared_ptr<IPCIntf> ipc_intf)
    : DisplayBase(display_id, kBuiltIn, event_handler, kDeviceBuiltIn, buffer_allocator,
                  comp_manager, hw_info_intf),
      ipc_intf_(ipc_intf) {}

DisplayBuiltIn::~DisplayBuiltIn() {
}

static uint64_t GetTimeInMs(struct timespec ts) {
  return (ts.tv_sec * 1000 + (ts.tv_nsec + 500000) / 1000000);
}

#ifndef TARGET_INCLUDES_NEO
DisplayError DisplayBuiltIn::SetupAiqe() {
  int value = 0;
  char value_str[200] = {0};
  aiqe::GetSsrcFeatureFactoryFp get_ssrc_feature_factory = nullptr;

  if (!prop_intf_) {
    DLOGI("Skipping AIQE setup. No panel feature property interface");
    return kErrorNone;
  }

  DebugHandler::Get()->GetProperty(AIQE_SSRC_ENABLE, &value);
  if (value == 1) {
    aiqe::SsrcFeatureFactory *ssrc_feature_factory = nullptr;
    aiqe::SsrcFeatureDisplayDetails *display_details = nullptr;
    std::string *default_mode = nullptr;
    bool *force_commit = nullptr;
    GenericPayload payload;

    if (!ssrc_lib_.Open(SSRC_LIBRARY_NAME)) {
      DLOGE("Unable to open library %s", SSRC_LIBRARY_NAME);
      return kErrorNotSupported;
    } else if (!ssrc_lib_.Sym(GET_SSRC_FF_INTF_NAME,
                              reinterpret_cast<void **>(&get_ssrc_feature_factory))) {
      DLOGE("Unable to get function pointer to retrieve AIQE feature factory");
      return kErrorNotSupported;
    }

    ssrc_feature_factory = get_ssrc_feature_factory();
    if (!ssrc_feature_factory) {
      DLOGE("Unable to retrieve SSRC feature factory");
      return kErrorNotSupported;
    }

    ssrc_feature_interface_ = ssrc_feature_factory->GetSsrcFeatureInterface(prop_intf_);
    if (!ssrc_feature_interface_) {
      DLOGE("Unable to retrieve SSRC feature interface");
      return kErrorNotSupported;
    }

    if (ssrc_feature_interface_->Init() != 0) {
      DLOGE("Unable to initalize SSRC feature interface");
      return kErrorNotSupported;
    }

    if (payload.CreatePayload(display_details) != 0) {
      DLOGE("Unable to create display details payload");
      return kErrorMemory;
    }

    display_details->panel_name = client_ctx_.hw_panel_info.panel_name;
    display_details->primary_panel = client_ctx_.hw_panel_info.is_primary_panel;
    switch (client_ctx_.mixer_attributes.split_type) {
      case kQuadSplit:
        display_details->ppc = 4;
        break;
      case kDualSplit:
        display_details->ppc = 2;
        break;
      case kNoSplit:
        display_details->ppc = 1;
        break;
      default:
        DLOGW("Unsupported mixer split - %d. Skipping AIQE Feature enablement",
              client_ctx_.mixer_attributes.split_type);
        return kErrorNotSupported;
    }

    if (ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureDisplayDetails, payload) != 0) {
      DLOGE("Unable to set display details on SSRC feature interface");
      return kErrorNotSupported;
    }

    payload.DeletePayload();
    if (payload.CreatePayload(default_mode) != 0) {
      DLOGE("Unable to create mode payload");
      return kErrorMemory;
    }

    DebugHandler::Get()->GetProperty(AIQE_SSRC_DEFAULT_MODE, value_str);
    if (strcmp(value_str, "") == 0) {
      DLOGI("Using default SSRC mode");
      *default_mode = "NORMAL/ON";
    } else {
      DLOGI("Using SSRC mode from vendor property - %s", value_str);
      *default_mode = value_str;
    }

    if (ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureModeId, payload) != 0) {
      DLOGE("Unable to set mode on AIQE SSRC feature interface");
      return kErrorNotSupported;
    }

    payload.DeletePayload();
    if (payload.CreatePayload(force_commit) != 0) {
      DLOGE("Unable to create commit payload");
      return kErrorMemory;
    }

    *force_commit = true;
    if (ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureCommitFeature, payload) != 0) {
      DLOGE("Unable to set commit on AIQE SSRC feature interface");
      return kErrorNotSupported;
    }

    ssrc_feature_enabled_ = true;
  } else {
    DLOGI("AIQE SSRC Enable property not set. Skipping SSRC enablement");
  }

  return kErrorNone;
}
#endif

DisplayError DisplayBuiltIn::Init() {
  ClientLock lock(disp_mutex_);

  DisplayError error = DPUCoreFactory::Create(display_id_info_, kBuiltIn, hw_info_intf_,
                                              buffer_allocator_, &dpu_core_mux_);
  if (error != kErrorNone) {
    DLOGE("Failed to create hardware interface on. Error = %d", error);
    return error;
  }

  dpu_core_mux_->GetHWInterface(&hw_intf_);

  if (-1 == display_id_info_.GetDisplayId()) {
    dpu_core_mux_->GetDisplayId(&display_id_);
    display_id_info_ = DisplayId(display_id_);
    core_id_ = display_id_info_.GetCoreIdMap();
    std::bitset<32> core_id_bitset = std::bitset<32>(core_id_);
    core_count_ = core_id_bitset.count();
  }

  error = DisplayBase::Init();
  if (error != kErrorNone) {
    dpu_core_mux_->Destroy();
    return error;
  }

  if (color_mgr_) {
    color_mgr_->ColorMgrGetStcModes(&stc_color_modes_);
  }

  if (client_ctx_.hw_panel_info.mode == kModeCommand && Debug::IsVideoModeEnabled()) {
    error = dpu_core_mux_->SetDisplayMode(kModeVideo);
    if (error != kErrorNone) {
      DLOGW("Retaining current display mode. Current = %d, Requested = %d",
            client_ctx_.hw_panel_info.mode, kModeVideo);
    }
  }

  std::vector<HWEvent> events;
#ifdef TRUSTED_VM
  events = {HWEvent::VSYNC,      HWEvent::EXIT,        HWEvent::PINGPONG_TIMEOUT,
            HWEvent::PANEL_DEAD, HWEvent::HW_RECOVERY, HWEvent::POWER_EVENT};
#else
  events = {HWEvent::VSYNC,
            HWEvent::EXIT,
            HWEvent::SHOW_BLANK_EVENT,
            HWEvent::THERMAL_LEVEL,
            HWEvent::PINGPONG_TIMEOUT,
            HWEvent::PANEL_DEAD,
            HWEvent::HW_RECOVERY,
            HWEvent::HISTOGRAM,
            HWEvent::BACKLIGHT_EVENT,
            HWEvent::POWER_EVENT,
            HWEvent::MMRM,
            HWEvent::VM_RELEASE_EVENT,
            HWEvent::VM_RECLAIM_EVENT};
  if ((client_ctx_.hw_panel_info.mode == kModeCommand) || client_ctx_.hw_panel_info.vhm_support) {
    events.push_back(HWEvent::IDLE_POWER_COLLAPSE);
  }
#endif
  std::bitset<8> core_id_map = display_id_info_.GetCoreIdMap();
  bool master_core = true;
  for (int i = 0; i < core_id_map.size(); i++) {
    if (!core_id_map[i]) {
      continue;
    }

    if (master_core) {
      event_list_[i] = events;
      primary_core_id_ = i;
      master_core = false;
    } else {
      // register panel dead and display event thread exit event for all the cores
      std::vector<HWEvent> core_event_list = {HWEvent::PANEL_DEAD};
      core_event_list.push_back(HWEvent::EXIT);
      event_list_[i] = core_event_list;
    }
  }

  error = HWEventsInterface::Create(display_id_info_, kBuiltIn, this, event_list_,
                                    &hw_events_intf_);
  if (error != kErrorNone) {
    DisplayBase::Deinit();
    dpu_core_mux_->Destroy();
    DLOGE("Failed to create hardware events interface on. Error = %d", error);
  }
  master_hw_events_intf_ = hw_events_intf_[primary_core_id_];

  // For CAC loopback case where CAC pipes are after DS blocks, These pipes take input w.r.t.
  // full panel resolution. In case of DS / Anamorphic compression usecase with cac loopback,
  // src_crop condition will not qualify for CAC pipes as buffer is smallar then panel size.
  // During CreateFbId for CAC pipes will fake it with dummy full screen buffer.
  error = AllocateDummyLoopbackCACBuffer();
  if (error != kErrorNone) {
    DLOGE("Failed to Get dummu loopback CAC buffer info");
    return error;
  }

  current_refresh_rate_ = client_ctx_.hw_panel_info.max_fps;

  int value = 0;
  Debug::Get()->GetProperty(ENABLE_HISTOGRAM_INTR, &value);
  if (value == 1) {
    initColorSamplingState();
  }

  value = 0;
  Debug::Get()->GetProperty(DEFER_FPS_FRAME_COUNT, &value);
  deferred_config_.frame_count = (value > 0) ? UINT32(value) : 0;

  value = 0;
  Debug::Get()->GetProperty(DISABLE_FBT_FOR_CWB_FALLBACK, &value);
  disable_fbt_for_cwb_fallback_ = (value == 1);
  DLOGI("disable_fbt_for_cwb_fallback_: %d", disable_fbt_for_cwb_fallback_);

  value = 0;
  Debug::Get()->GetProperty(ENABLE_HFI_PATH, &value);
  hfi_path_supported_ = (value > 0);

  error = event_proxy_info_.Init(client_ctx_.hw_panel_info.panel_name, this, extension_lib_,
                                 prop_intf_);
  if (error != kErrorNone) {
    DLOGW("Failed to initialize event proxy info");
    event_proxy_info_.Deinit();
  }

  if (pf_factory_ && prop_intf_) {
    // Get status of RC enablement property. Default RC is disabled.
    int rc_prop_value = 0;
    Debug::GetProperty(ENABLE_ROUNDED_CORNER, &rc_prop_value);
    if (rc_prop_value && EnableRC()) {
      rc_enable_prop_ = true;
    }
    DLOGI("RC feature %s on %s for display %d-%d",
          rc_enable_prop_ ? "enabled" : "disabled",
          client_ctx_.hw_panel_info.is_primary_panel ? "primary" : "secondary",
          display_id_, display_type_);

    if ((error = SetupSPR()) != kErrorNone) {
      DLOGE("SPR Failed to initialize. Error = %d", error);
      DisplayBase::Deinit();
      dpu_core_mux_->Destroy();
      HWEventsInterface::Destroy(&hw_events_intf_);
      master_hw_events_intf_ = nullptr;
      return error;
    }

    if ((error = HandleSPR()) != kErrorNone) {
      DLOGE("Failed to get SPR status. Error = %d", error);
      DisplayBase::Deinit();
      HWInterface::Destroy(hw_intf_);
      HWEventsInterface::Destroy(&hw_events_intf_);
      master_hw_events_intf_ = nullptr;
      return error;
    }

    int enable_abc = 0;
    Debug::Get()->GetProperty(ENABLE_ABC, &enable_abc);
    abc_prop_ = (enable_abc > 0);

    abc_tvm_enabled_ = (enable_abc == 2);

#ifdef TRUSTED_VM
    abc_prop_ = abc_tvm_enabled_;
#endif

    // Get demura count from HW info
    uint32_t demura_cnt = 0;
    for (int i = 0; i < core_count_; i++) {
      demura_cnt = std::max(demura_cnt, hw_resource_info_[i].demura_count);
    }
    // Disable demura only when demura block is available
    if (demura_cnt > 0) {
      DisableDemuraForHandOff();
    }

    int demura_prop = 0;
    Debug::Get()->GetProperty(ENABLE_DEMURA, &demura_prop);
    int disable_demura_prop = 0;
    if (IsPrimaryDisplay()) {
      Debug::Get()->GetProperty(DISABLE_DEMURA_PRIMARY, &disable_demura_prop);
    } else {
      Debug::Get()->GetProperty(DISABLE_DEMURA_SECONDARY, &disable_demura_prop);
    }
    demura_enable_ = demura_prop && (!disable_demura_prop);

    if (demura_enable_) {  // Create parser manager for demura
      pm_intf_ = pf_factory_->CreateDemuraParserManager(ipc_intf_, buffer_allocator_);
      if (!pm_intf_) {
        DLOGE("Failed to create Parser Manager intf");
      } else if (pm_intf_->Init() != 0) {
        DLOGE("Failed to init Parser Manager intf");
        pm_intf_->Deinit();
        pm_intf_ = nullptr;
      }
    }

#ifndef TRUSTED_VM
    std::thread([=] { DisplayBuiltIn::StartTvmServices(); }).detach();
#endif

    if (abc_prop_) {
      SetupABC();
    } else {
      if (!demura_enable_) {
        comp_manager_->FreeDemuraFetchResources(display_id_);
        comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
      } else {
        SetupDemuraT0AndTn();
      }
    }

  } else {
    DLOGW("Skipping Panel Feature Setups!");
  }
  value = 0;
  DebugHandler::Get()->GetProperty(DISABLE_DYNAMIC_FPS, &value);
  disable_dyn_fps_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(ENABLE_QSYNC_IDLE, &value);
  enable_qsync_idle_ = client_ctx_.hw_panel_info.qsync_support && (value == 1);
  if (enable_qsync_idle_) {
    DLOGI("Enabling qsync on idling");

    if (client_ctx_.hw_panel_info.transfer_time_us_min) {
      DLOGI("Setting transfer time to min: %d", client_ctx_.hw_panel_info.transfer_time_us_min);
      UpdateTransferTime(client_ctx_.hw_panel_info.transfer_time_us_min);
    }
  }

  value = 0;
  Debug::Get()->GetProperty(ENABLE_AI_SCALER_PROP, &value);
  enable_ai_scaler_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(ENHANCE_IDLE_TIME, &value);
  enhance_idle_time_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(ENABLE_DPPS_DYNAMIC_FPS, &value);
  enable_dpps_dyn_fps_ = (value == 1);

  value = 0;
  Debug::Get()->GetProperty(DISABLE_NOISE_LAYER, &value);
  noise_disable_prop_ = (value == 1);
  DLOGI("Noise Layer Feature is %s for display = %d-%d", noise_disable_prop_ ? "Disabled" :
        "Enabled", display_id_, display_type_);

  value = 0;
  DebugHandler::Get()->GetProperty(DISABLE_CWB_IDLE_FALLBACK, &value);
  disable_cwb_idle_fallback_ = (value == 1);

  value = 0;
  DebugHandler::Get()->GetProperty(ENABLE_BRIGHTNESS_DRM_PROP, &value);
  enable_brightness_drm_prop_ = (value == 1);

#ifdef TRUSTED_VM
  disable_cwb_idle_fallback_ = 1;
#endif

  if (!disable_cwb_idle_fallback_) {
    value = 0;
    Debug::Get()->GetProperty(IDLE_FALLBACK_ON_DSPP, &value);
    idle_fallback_on_dspp_ = (value == 1);
  }

  value = 0;
  DebugHandler::Get()->GetProperty(FORCE_LM_TO_FB_CONFIG, &value);
  force_lm_to_fb_config_ = (value == 1);

  value = 0;
  Debug::Get()->GetProperty(ENABLE_PRIVACY_LAYERS, &value);
  // TODO(user): Enable privacy filter for dual dpu, then update this check
  if (value == 1 && core_count_ == 1) {
    uint32_t max_privacy_regions = hw_intf_->GetMaxPrivacyRegionsSupported();

    if (max_privacy_regions > 0) {
      privacy_region_mgr_ = new PrivacyRegionManager(max_privacy_regions);
    }
  }

  NoiseInit();
  InitCWBBuffer();
#ifndef TARGET_INCLUDES_NEO
  SetupAiqe();
#endif

  left_frame_roi_.resize(core_count_);
  right_frame_roi_.resize(core_count_);

  return error;
}

DisplayError DisplayBuiltIn::Deinit() {
  {
    ClientLock lock(disp_mutex_);

    if (demura_) {
      SetDemuraIntfStatus(false);

      if (demura_->Deinit() != 0) {
        DLOGE("Unable to DeInit Demura on Display %d-%d", display_id_, display_type_);
      }
    }

    if (demuratn_) {
      EnableDemuraTn(false);
      if (demuratn_->Deinit() != 0) {
        DLOGE("Unable to DeInit DemuraTn on Display %d", display_id_);
      }
      demuratn_override_feature_ = kFeatureMax;
    }
    if (demuratn_cleanup_intf_) {
      if (demuratn_cleanup_intf_->Deinit() != 0) {
        DLOGE("Unable to DeInit demuratn_cleanup_intf_ on Display %d", display_id_);
      }
    }
    demura_dynamic_enabled_ = true;

    DeinitCWBBuffer();
    CloseFd(&prev_framebuffer_.planes[0].fd);
    CloseFd(&curr_framebuffer_.planes[0].fd);
    hw_rc_blocks_in_use_ -= rc_blocks_reserved_;

    if (service_manager_intf_) {
      service_manager_intf_->Deinit();
      service_manager_intf_.reset();
      service_manager_intf_ = nullptr;
    }

    if (vm_file_xfer_intf_) {
      vm_file_xfer_intf_->Deinit();
      vm_file_xfer_intf_.reset();
      vm_file_xfer_intf_ = nullptr;
    }

    if (demura_enable_) {
      if (pm_intf_ && pm_intf_.use_count() == 1) {
        GenericPayload dummy;
        int ret = pm_intf_->SetParameter(kDemuraParserManagerParamReleaseParsers, dummy);
        if (ret < 0) {
          DLOGW("Failed to release demura parsers");
        }
        pm_intf_->Deinit();
      }
    }

    if (feat_license_intf_) {
      feat_license_intf_->Deinit();
      feat_license_intf_.reset();
      feat_license_intf_ = nullptr;
    }
  }

  dpps_info_.Deinit();
  event_proxy_info_.Deinit();
  return DisplayBase::Deinit();
}

DisplayError DisplayBuiltIn::PrePrepare(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  uint32_t new_mixer_width = 0;
  uint32_t new_mixer_height = 0;
  uint32_t display_width = client_ctx_.display_attributes.x_pixels;
  uint32_t display_height = client_ctx_.display_attributes.y_pixels;
#ifndef TARGET_INCLUDES_NEO
  GenericPayload bool_payload;
  bool *force_update = nullptr;
#endif

  DisplayError error = HandleDemuraLayer(layer_stack);
  if (error != kErrorNone) {
    return error;
  }

  error = HandleSPR();
  if (error != kErrorNone) {
    return error;
  }
  disp_layer_stack_->stack_info.spr_enable = spr_enable_;

  AppendCWBLayer(layer_stack);
  // Do not skip validate if needs update PP features.
  if (color_mgr_) {
    needs_validate_ |= color_mgr_->IsValidateNeeded();
  }

  error = DisplayBase::PrePrepare(layer_stack);
  if (error == kErrorNone || error == kErrorNeedsLutRegen) {
    return error;
  }

  if (NeedsMixerReconfiguration(layer_stack, &new_mixer_width, &new_mixer_height)) {
    error = ReconfigureMixer(new_mixer_width, new_mixer_height);
    mixer_resolution_updated_ = (error == kErrorNone);
    if (error != kErrorNone) {
      ReconfigureMixer(display_width, display_height);
    }
  } else {
    if (CanSkipDisplayPrepare(layer_stack)) {
      UpdateQsyncConfig();
      return kErrorNone;
    }
  }
  error = ChangeFps();
  lower_fps_ = disp_layer_stack_->stack_info.lower_fps;

  if (color_mgr_ && client_ctx_.hw_panel_info.mode == kModeVideo && idle_fallback_on_dspp_) {
    CwbTapPoint tap_point = CwbTapPoint::kDsppTapPoint;
    bool destination_scaler =
        (client_ctx_.display_attributes.x_pixels != client_ctx_.mixer_attributes.width ||
         client_ctx_.display_attributes.y_pixels != client_ctx_.mixer_attributes.height);
    tap_point = destination_scaler ? CwbTapPoint::kLmTapPoint : CwbTapPoint::kDsppTapPoint;
    if (tap_point == CwbTapPoint::kDsppTapPoint) {
      color_mgr_->ColorMgrIdleFallback(lower_fps_);
      needs_validate_ |= color_mgr_->IsValidateNeeded();
    }
  }

#ifndef TARGET_INCLUDES_NEO
  if (ssrc_feature_enabled_) {
    if (bool_payload.CreatePayload(force_update) != 0) {
      DLOGE("Unable to create force update payload");
      return kErrorMemory;
    }

    *force_update = false;
    if (ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureCommitFeature, bool_payload) != 0) {
      DLOGE("Unable to set commit on SSRC feature interface");
      return kErrorNotSupported;
    }
  }
#endif

  return kErrorNotValidated;
}

DisplayError DisplayBuiltIn::HandleSPR() {
  if (spr_) {
    GenericPayload out;
    uint32_t *enable = nullptr;
    int ret = out.CreatePayload<uint32_t>(enable);
    if (ret) {
      DLOGE("Failed to create the payload. Error:%d", ret);
      validated_ = false;
      return kErrorUndefined;
    }
    ret = spr_->GetParameter(kSPRFeatureEnable, &out);
    if (ret) {
      DLOGE("Failed to get the spr status. Error:%d", ret);
      validated_ = false;
      return kErrorUndefined;
    }
    spr_enable_ = *enable;
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::Prepare(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  ClientLock lock(disp_mutex_);

  DisplayError error = PrePrepare(layer_stack);
  if (error == kErrorNone) {
    return kErrorNone;
  }

  if (error == kErrorNeedsLutRegen && (ForceToneMapUpdate(layer_stack) == kErrorNone)) {
    return kErrorNone;
  }

  error = DisplayBase::Prepare(layer_stack);
  if (error != kErrorNone) {
    return error;
  } else {
    if (!disp_layer_stack_->stack->flags.default_strategy && previous_frame_default_strategy_) {
      DLOGI("Strategy has been changed to extern support from default, enabling demura");
      previous_frame_default_strategy_ = false;
      if (demura_intended_) {
        int ret = SetDemuraIntfStatus(true);
        if (ret) {
          DLOGE("Failed to set demura intf status. Error:%d", ret);
        }
      }
    }
    if (disp_layer_stack_->stack->flags.default_strategy) {
      previous_frame_default_strategy_ = true;
    }
  }

  UpdateQsyncConfig();

  CacheFrameROI();

  NotifyDppsHdrPresent(layer_stack);

  pending_commit_ = true;

  return kErrorNone;
}

void DisplayBuiltIn::NotifyDppsHdrPresent(LayerStack *layer_stack) {
  if (hdr_present_ != layer_stack->flags.hdr_present) {
    hdr_present_ = layer_stack->flags.hdr_present;
    DLOGV_IF(kTagDisplay, "Notify DPPS hdr_present %d on display %d-%d", hdr_present_,
             display_id_, display_type_);
    DppsNotifyPayload info = {};
    info.is_primary = IsPrimaryDisplay();
    info.payload = &hdr_present_;
    info.payload_size = sizeof(hdr_present_);
    dpps_info_.DppsNotifyOps(kDppsHdrPresentEvent, &info, sizeof(info));
  }
}

void DisplayBuiltIn::CacheFrameROI() {
  for (int i = 0; i < core_count_; i++) {
    uint32_t core_id = hw_resource_info_[i].core_id;
    left_frame_roi_[i] = {};
    right_frame_roi_[i] = {};
    if (disp_layer_stack_->info.at(core_id).left_frame_roi.size() &&
      disp_layer_stack_->info.at(core_id).right_frame_roi.size()) {
      left_frame_roi_[i] = disp_layer_stack_->info.at(core_id).left_frame_roi.at(0);
      right_frame_roi_[i] = disp_layer_stack_->info.at(core_id).right_frame_roi.at(0);
    }
  }
}

void DisplayBuiltIn::UpdateQsyncConfig() {
  // QSync and AVR Step features are de-coupled on CMD Mode panel.
  if ((client_ctx_.hw_panel_info.mode == kModeVideo) && !client_ctx_.hw_panel_info.qsync_support) {
    return;
  }

  // Get qsync min fps for the current mode
  uint32_t qsync_mode_min_fps = 0;
  dpu_core_mux_->GetQsyncFps(&qsync_mode_min_fps);
  QSyncMode mode = kQSyncModeNone;
  if (!qsync_mode_min_fps) {
    // Set qsync mode to 0 when the current mode doesn't support it.
    mode = kQSyncModeNone;
    DLOGV_IF(kTagDisplay, "Qsync disabled as current mode doesn't support it");
  } else if (lower_fps_ && enable_qsync_idle_) {
    // Override to continuous mode upon idling.
    mode = kQSyncModeContinuous;
    DLOGV_IF(kTagDisplay, "Qsync entering continuous mode");
  } else {
    // Set Qsync mode requested by client.
    mode = qsync_mode_;
    DLOGV_IF(kTagDisplay, "Restoring display %d-%d client's qsync mode: %d", display_id_,
             display_type_, mode);
  }

  disp_layer_stack_->stack_info.common_info.hw_avr_info.update = needs_avr_update_;
  if (mode != active_qsync_mode_) {
    disp_layer_stack_->stack_info.common_info.hw_avr_info.update.set(kUpdateAVRModeFlag);
  }
  disp_layer_stack_->stack_info.common_info.hw_avr_info.mode = GetAvrMode(mode);
  disp_layer_stack_->stack_info.common_info.hw_avr_info.step_enabled = avr_step_enabled_;

  DLOGV_IF(kTagDisplay, "display %d-%u update: %llu mode: %d AVR Step state: %d", display_id_,
           display_type_, disp_layer_stack_->stack_info.common_info.hw_avr_info.update.to_ullong(), mode,
           avr_step_enabled_);

  // Store active mode.
  active_qsync_mode_ = mode;
}

void DisplayBuiltIn::HandleUpdateTransferTime(QSyncMode mode) {
  if (mode == kQSyncModeNone) {
    DLOGI("Qsync mode set to %d successfully, setting transfer time to min: %d", mode,
          client_ctx_.hw_panel_info.transfer_time_us_min);
    UpdateTransferTime(client_ctx_.hw_panel_info.transfer_time_us_min);
  } else {
    DLOGI("Qsync mode set to %d successfully, setting transfer time to max: %d", mode,
          client_ctx_.hw_panel_info.transfer_time_us_max);
    UpdateTransferTime(client_ctx_.hw_panel_info.transfer_time_us_max);
  }
}

HWAVRModes DisplayBuiltIn::GetAvrMode(QSyncMode mode) {
  switch (mode) {
     case kQSyncModeNone:
       return kQsyncNone;
     case kQSyncModeContinuous:
       return kContinuousMode;
     case kQsyncModeOneShot:
     case kQsyncModeOneShotContinuous:
       return kOneShotMode;
     default:
       return kQsyncNone;
  }
}

void DisplayBuiltIn::initColorSamplingState() {
  samplingState = SamplingState::Off;
  histogramCtrl.object_type = DRM_MODE_OBJECT_CRTC;
  histogramCtrl.feature_id = sde_drm::DRMDPPSFeatureID::kFeatureAbaHistCtrl;
  histogramCtrl.value = sde_drm::HistModes::kHistDisabled;

  histogramIRQ.object_type = DRM_MODE_OBJECT_CRTC;
  histogramIRQ.feature_id = sde_drm::DRMDPPSFeatureID::kFeatureAbaHistIRQ;
  histogramIRQ.value = sde_drm::HistModes::kHistDisabled;
  histogramSetup = true;
}

DisplayError DisplayBuiltIn::setColorSamplingState(SamplingState state) {
  samplingState = state;
  if (samplingState == SamplingState::On) {
    histogramCtrl.value = sde_drm::HistModes::kHistEnabled;
    histogramIRQ.value = sde_drm::HistModes::kHistEnabled;
    if (client_ctx_.hw_panel_info.mode == kModeCommand) {
      uint32_t pending;
      ControlPartialUpdate(false /* enable */, &pending);
    }
  } else {
    histogramCtrl.value = sde_drm::HistModes::kHistDisabled;
    histogramIRQ.value = sde_drm::HistModes::kHistDisabled;
    if (client_ctx_.hw_panel_info.mode == kModeCommand) {
      uint32_t pending;
      ControlPartialUpdate(true /* enable */, &pending);
    }
  }

  // effectively drmModeAtomicAddProperty for the SDE_DSPP_HIST_CTRL_V1
  return DppsProcessOps(kDppsSetFeature, &histogramCtrl, sizeof(histogramCtrl));
}

DisplayError DisplayBuiltIn::colorSamplingOn() {
  if (!histogramSetup) {
    return kErrorParameters;
  }
  return setColorSamplingState(SamplingState::On);
}

DisplayError DisplayBuiltIn::colorSamplingOff() {
  if (!histogramSetup) {
    return kErrorParameters;
  }
  return setColorSamplingState(SamplingState::Off);
}

DisplayError DisplayBuiltIn::SetupSPR() {
  int spr_prop_value = 0;
  int spr_bypass_prop_value = 0;
  int spr_disable_value = 0;
  Debug::GetProperty(ENABLE_SPR, &spr_prop_value);

  if (IsPrimaryDisplay()) {
    Debug::Get()->GetProperty(DISABLE_SPR_PRIMARY, &spr_disable_value);
    Debug::GetProperty(ENABLE_SPR_BYPASS, &spr_bypass_prop_value);
  } else {
    Debug::Get()->GetProperty(DISABLE_SPR_SECONDARY, &spr_disable_value);
    Debug::GetProperty(ENABLE_SPR_BYPASS_SECONDARY, &spr_bypass_prop_value);
  }

  if (spr_prop_value && !spr_disable_value) {
    SPRInputConfig spr_cfg;
    spr_cfg.panel_name = std::string(client_ctx_.hw_panel_info.panel_name);
    spr_cfg.spr_bypassed = (spr_bypass_prop_value) ? true : false;
    spr_ = pf_factory_->CreateSPRIntf(spr_cfg, prop_intf_);

    if (spr_ == nullptr) {
      DLOGE("Failed to create SPR interface");
      return kErrorResources;
    }

    if (spr_->Init() != 0) {
      DLOGE("Failed to initialize SPR");
      return kErrorResources;
    }

    spr_bypassed_ = spr_cfg.spr_bypassed;
    if (color_mgr_) {
      color_mgr_->ColorMgrSetSprIntf(spr_);
    }
    comp_manager_->SetSprIntf(display_comp_ctx_, spr_);
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupDemura(int current_idx) {
  DemuraInputConfig input_cfg;
  input_cfg.secure_session = false;  // TODO(user): Integrate with secure solution
  std::string brightness_base;
  dpu_core_mux_->GetPanelBrightnessBasePath(&brightness_base);
  input_cfg.brightness_path = brightness_base+"brightness";

  std::vector<FetchResourceList> frlv;
  frlv.resize(core_count_);
  comp_manager_->GetDemuraFetchResources(display_comp_ctx_, &frlv);
  // ToDo(devanshi) : handle for all DPU instead of DPU 0
  auto frl = frlv[0];
  for (auto &fr : frl) {
    int i = std::get<1>(fr);  // fetch resource index
    input_cfg.resources.set(i);
  }

#ifdef TRUSTED_VM
  input_cfg.secure_session = true;
#endif
  input_cfg.panel_id = panel_id_;
  DLOGI("panel id %" PRIX64 "\n", input_cfg.panel_id);
  input_cfg.panel_name = client_ctx_.hw_panel_info.panel_name;
  input_cfg.display_intf = this;
  std::unique_ptr<DemuraIntf> demura =
      pf_factory_->CreateDemuraIntf(input_cfg, prop_intf_, buffer_allocator_, spr_);
  if (!demura) {
    DLOGE("Unable to create Demura on Display %d-%d", display_id_, display_type_);
    return kErrorMemory;
  }

  demura_ = std::move(demura);
  if (demura_->Init() != 0) {
    DLOGE("Unable to initialize Demura on Display %d-%d", display_id_, display_type_);
    return kErrorUndefined;
  }

  if (SetupCorrectionLayer() != kErrorNone) {
    DLOGE("Unable to setup Demura layer on Display %d-%d", display_id_, display_type_);
    return kErrorUndefined;
  }

  if (SetDemuraIntfStatus(true, current_idx)) {
    return kErrorUndefined;
  }

  comp_manager_->SetDemuraStatusForDisplay(display_id_, true);
  demura_intended_ = true;
  DLOGI("Enabled Demura Core!");

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupCorrectionLayer() {
  if (abc_prop_) {
    return SetupABCLayer();
  } else {
    return SetupDemuraLayer();
  }
}

DisplayError DisplayBuiltIn::SetupDemuraLayer() {
  int ret = 0;
  GenericPayload pl;

  DemuraCorrectionSurfaces *corrdata = nullptr;
  if ((ret = pl.CreatePayload<DemuraCorrectionSurfaces>(corrdata))) {
    DLOGE("Failed to create payload for BufferInfo, error = %d", ret);
    return kErrorResources;
  }

  if ((ret = demura_->GetParameter(kDemuraFeatureParamCorrectionBuffer, &pl))) {
    DLOGE("Failed to get BufferInfo, error = %d", ret);
    return kErrorResources;
  }

  GenericPayload dec_pl;
  DemuraHfcDecimationInfo *hfc_decimate_info = nullptr;
  if ((ret = dec_pl.CreatePayload<DemuraHfcDecimationInfo>(hfc_decimate_info))) {
    DLOGE("Failed to create payload for decimate_cfg, error = %d", ret);
    return kErrorResources;
  }

  if ((ret = demura_->GetParameter(kDemuraFeatureParamHfcDecimationInfo, &dec_pl))) {
    DLOGE("Failed to get decimate_cfg, error = %d", ret);
    return kErrorResources;
  }

  demura_layer_.clear();  // This will clear the old demura layers

  for (int buf_idx = 0; buf_idx < corrdata->surfaces.size(); buf_idx++) {
    if (!corrdata->valid[buf_idx])
      continue;
    Layer demura_layer = {};
#ifndef TRUSTED_VM
    demura_layer.input_buffer.buffer_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
    demura_layer.input_buffer.handle_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
#endif
    demura_layer.input_buffer.size = corrdata->surfaces[buf_idx].alloc_buffer_info.size;
    demura_layer.input_buffer.format = corrdata->surfaces[buf_idx].alloc_buffer_info.format;
    demura_layer.input_buffer.width = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.unaligned_width =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.height = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.unaligned_height =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].fd = corrdata->surfaces[buf_idx].alloc_buffer_info.fd;
    demura_layer.input_buffer.planes[0].stride =
        corrdata->surfaces[buf_idx].alloc_buffer_info.stride;
    hfc_buffer_width_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    hfc_buffer_height_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].offset = 0;
    demura_layer.input_buffer.flags.demura = 1;
    demura_layer.composition = kCompositionDemura;
    demura_layer.blending = kBlendingSkip;
    demura_layer.flags.is_demura = 1;
    demura_layer.demura_decimate_w = hfc_decimate_info->decimate_w;
    demura_layer.demura_decimate_h = hfc_decimate_info->decimate_h;
    // ROI must match input dimensions
    demura_layer.src_rect.top = 0;
    demura_layer.src_rect.left = 0;
    demura_layer.src_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.src_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura src: ", demura_layer.src_rect);
    demura_layer.dst_rect.top = 0;
    demura_layer.dst_rect.left = 0;
    demura_layer.dst_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.dst_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura dst: ", demura_layer.dst_rect);
    demura_layer.buffer_map = std::make_shared<LayerBufferMap>();
    demura_layer_.push_back(demura_layer);
  }
  return kErrorNone;
}

DisplayError DisplayBuiltIn::DumpDemuraSurface(const char *dir_path, uint32_t frame_index) {
  ClientLock lock(disp_mutex_);

  if (demura_layer_.empty()) {
    DLOGI("No demura layer present");
    return kErrorNone;
  }

  for (int i = 0; i < demura_layer_.size(); i++) {
    if (demura_layer_[i].input_buffer.planes[0].fd > 0 && demura_layer_[i].input_buffer.size) {
      void *mapped_buffer = mmap(NULL, demura_layer_[i].input_buffer.size, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, demura_layer_[i].input_buffer.planes[0].fd, 0);
      if (mapped_buffer == MAP_FAILED) {
        DLOGE("mmap failed with err %s", strerror(errno));
        return kErrorUndefined;
      }

      if (!mapped_buffer) {
        DLOGE("mapped buffer is empty");
        return kErrorUndefined;
      }

      char dump_file_name[PATH_MAX];
      snprintf(dump_file_name, sizeof(dump_file_name),
               "%s/input_layer_demura%d_%dx%d_%s_frame%d.raw", dir_path, i, hfc_buffer_width_,
               hfc_buffer_height_, GetFormatString(demura_layer_[i].input_buffer.format),
               frame_index);

      FILE *fp = fopen(dump_file_name, "w+");
      size_t result = 0;
      if (fp) {
        result = fwrite(mapped_buffer, demura_layer_[i].input_buffer.size, 1, fp);
        fclose(fp);
      }

      DLOGI("Frame Dump %s: is %s", dump_file_name, result ? "Successful" : "Failed");
      munmap(mapped_buffer, demura_layer_[i].input_buffer.size);
    }
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupABCLayer() {
  int ret = 0;
  GenericPayload pl;

  DemuraCorrectionSurfaces *corrdata = nullptr;
  if ((ret = pl.CreatePayload<DemuraCorrectionSurfaces>(corrdata))) {
    DLOGE("Failed to create payload for BufferInfo, error = %d", ret);
    return kErrorResources;
  }

  if ((ret = demura_->GetParameter(kDemuraFeatureParamCorrectionBuffer, &pl))) {
    DLOGE("Failed to get BufferInfo, error = %d", ret);
    return kErrorResources;
  }
  demura_layer_.clear();  // This will clear the old abc layers

  for (int buf_idx = 0; buf_idx < corrdata->surfaces.size(); buf_idx++) {
    if (!corrdata->valid[buf_idx])
      continue;
    Layer demura_layer = {};
    demura_layer.input_buffer.size = corrdata->surfaces[buf_idx].alloc_buffer_info.size;
#ifndef TRUSTED_VM
    demura_layer.input_buffer.buffer_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
    demura_layer.input_buffer.handle_id = corrdata->surfaces[buf_idx].alloc_buffer_info.id;
#endif
    demura_layer.input_buffer.format = corrdata->surfaces[buf_idx].alloc_buffer_info.format;
    demura_layer.input_buffer.width = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.unaligned_width =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    demura_layer.input_buffer.height = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.unaligned_height =
        corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].fd = corrdata->surfaces[buf_idx].alloc_buffer_info.fd;
    demura_layer.input_buffer.planes[0].stride =
        corrdata->surfaces[buf_idx].alloc_buffer_info.stride;
    hfc_buffer_width_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_width;
    hfc_buffer_height_ = corrdata->surfaces[buf_idx].alloc_buffer_info.aligned_height;
    demura_layer.input_buffer.planes[0].offset = 0;
    demura_layer.input_buffer.flags.demura = 1;
    demura_layer.composition = kCompositionDemura;
    demura_layer.blending = kBlendingSkip;
    demura_layer.flags.is_abc = 1;
    // ROI must match input dimensions
    demura_layer.src_rect.top = 0;
    demura_layer.src_rect.left = 0;
    demura_layer.src_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.src_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura src: ", demura_layer.src_rect);
    demura_layer.dst_rect.top = 0;
    demura_layer.dst_rect.left = 0;
    demura_layer.dst_rect.right = corrdata->surfaces[buf_idx].buffer_config.width;
    demura_layer.dst_rect.bottom = corrdata->surfaces[buf_idx].buffer_config.height;
    LogI(kTagNone, "Demura dst: ", demura_layer.dst_rect);
    demura_layer.buffer_map = std::make_shared<LayerBufferMap>();
    demura_layer_.push_back(demura_layer);
  }
  return kErrorNone;
}

void DisplayBuiltIn::PreCommit(LayerStack *layer_stack) {
  uint32_t app_layer_count = disp_layer_stack_->stack_info.app_layer_count;

  // Enabling auto refresh is async and needs to happen before commit ioctl
  if (client_ctx_.hw_panel_info.mode == kModeCommand) {
    bool enable = (app_layer_count == 1) && layer_stack->flags.single_buffered_layer_present;
    bool need_refresh = layer_stack->flags.single_buffered_layer_present && (app_layer_count > 1);

    dpu_core_mux_->SetAutoRefresh(enable);
    if (need_refresh) {
      event_handler_->Refresh();
    }
  }

  if (trigger_mode_debug_ != kFrameTriggerMax) {
    DisplayError error = dpu_core_mux_->SetFrameTrigger(trigger_mode_debug_);
    if (error != kErrorNone) {
      DLOGE("Failed to set frame trigger mode %d, err %d", (int)trigger_mode_debug_, error);
    } else {
      DLOGV_IF(kTagDisplay, "Set frame trigger mode %d on display %d-%d", trigger_mode_debug_,
               display_id_, display_type_);
      trigger_mode_debug_ = kFrameTriggerMax;
    }
  }

  // effectively drmModeAtomicAddProperty for SDE_DSPP_HIST_IRQ_V1
  if (histogramSetup) {
    SetDppsFeatureLocked(&histogramIRQ, sizeof(histogramIRQ));
  }
}

DisplayError DisplayBuiltIn::SetupABCFeature() {
  DemuraInputConfig input_cfg;
  input_cfg.secure_session = false;
  std::string brightness_base;
  hw_intf_->GetPanelBrightnessBasePath(&brightness_base);
  input_cfg.brightness_path = brightness_base + "brightness";

  std::vector<FetchResourceList> frlv;
  frlv.resize(core_count_);
  comp_manager_->GetDemuraFetchResources(display_comp_ctx_, &frlv);
  // handled for DPU 0
  auto frl = frlv[0];
  for (auto &fr : frl) {
    int i = std::get<1>(fr);  // fetch resource index
    input_cfg.resources.set(i);
  }

#ifdef TRUSTED_VM
  input_cfg.secure_session = true;
#endif
  input_cfg.panel_id = panel_id_;
  input_cfg.panel_width = client_ctx_.display_attributes.x_pixels;
  input_cfg.panel_height = client_ctx_.display_attributes.y_pixels;
  input_cfg.panel_name = std::string(client_ctx_.hw_panel_info.panel_name);
  for (auto it = input_cfg.panel_name.begin(); it != input_cfg.panel_name.end(); ++it) {
    if (*it == ' ') {
      *it = '_';
    }
  }
  DLOGI("ABC panel id %" PRIX64 " actual panel-name %s\n", input_cfg.panel_id,
        input_cfg.panel_name.c_str());
  if (!abc_factory_) {
    DLOGE("Failed to get ABC feature Factory");
    return kErrorResources;
  }

  std::unique_ptr<DemuraIntf> abc_intf =
      abc_factory_->CreateABCIntf(input_cfg, prop_intf_, buffer_allocator_, this);
  if (!abc_intf) {
    DLOGE("Unable to create abc_intf on Display %d-%d", display_id_, display_type_);
    return kErrorMemory;
  }

  demura_ = std::move(abc_intf);
  if (demura_->Init() != 0) {
    DLOGE("Unable to initialize abc_intf on Display %d-%d", display_id_, display_type_);
    comp_manager_->FreeDemuraFetchResources(display_id_);
    demura_->Deinit();
    demura_.reset();
    return kErrorUndefined;
  }

  if (SetDemuraIntfStatus(true)) {
    DLOGE("Failed to set ABC Status on Display %d", display_id_);
    return kErrorUndefined;
  }

  comp_manager_->SetDemuraStatusForDisplay(display_id_, true);
  abc_enabled_ = true;
  DLOGI("Enabled ABC Core!");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupABC() {
  DisplayError error = kErrorNone;
  uint64_t ret = 0, panel_id = 0;
  int value = 0;

  // if ABC is not set during resourse reservation
  if (!comp_manager_->GetDemuraStatus()) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    return kErrorNone;
  }

  if (IsPrimaryDisplay()) {
    Debug::Get()->GetProperty(DISABLE_ABC_PRIMARY, &value);
    DLOGI("primary panel id value %" PRIX64 "\n", panel_id);
  } else {
    Debug::Get()->GetProperty(DISABLE_ABC_SECONDARY, &value);
    DLOGI("secondary panel id value %" PRIX64 "\n", panel_id);
  }

  if (value > 0) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    return kErrorNone;
  } else if (value < 0) {
    return kErrorUndefined;
  }

  PanelFeaturePropertyInfo info;
  if (!panel_id) {
    info.prop_ptr = reinterpret_cast<uint64_t>(&panel_id);
    info.prop_id = kPanelFeatureDemuraPanelId;
    ret = prop_intf_->GetPanelFeature(&info);
    if (ret) {
      DLOGE("Failed to get panel id, error = %" PRIu64, ret);
      return kErrorUndefined;
    }
  }
  panel_id_ = panel_id;

  error = SetupABCFeature();
  if (error != kErrorNone) {
    // Non-fatal but not expected, log error
    DLOGE("Mdnie B failed to initialize on display %d-%d, Error = %d", display_id_, display_type_,
          error);
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    if (demura_) {
      SetDemuraIntfStatus(false);
    }
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupDemuraT0AndTn() {
  DisplayError error = kErrorNone;
  int ret = 0, panel_id_w = 0;
  uint64_t panel_id = 0;

  if (IsPrimaryDisplay()) {
    Debug::Get()->GetProperty(DEMURA_PRIMARY_PANEL_OVERRIDE_LOW, &panel_id_w);
    panel_id = static_cast<uint32_t>(panel_id_w);
    Debug::Get()->GetProperty(DEMURA_PRIMARY_PANEL_OVERRIDE_HIGH, &panel_id_w);
    panel_id |= ((static_cast<uint64_t>(panel_id_w)) << 32);
    DLOGI("panel overide total value for primary display %" PRIX64 "\n", panel_id);
  } else {
    Debug::Get()->GetProperty(DEMURA_SECONDARY_PANEL_OVERRIDE_LOW, &panel_id_w);
    panel_id = static_cast<uint32_t>(panel_id_w);
    Debug::Get()->GetProperty(DEMURA_SECONDARY_PANEL_OVERRIDE_HIGH, &panel_id_w);
    panel_id |= ((static_cast<uint64_t>(panel_id_w)) << 32);
    DLOGI("panel overide total value for secondary display %" PRIX64 "\n", panel_id);
  }

  PanelFeaturePropertyInfo info;
  if (!panel_id) {
    info.prop_ptr = reinterpret_cast<uint64_t>(&panel_id);
    info.prop_id = kPanelFeatureDemuraPanelId;
    ret = prop_intf_->GetPanelFeature(&info);
    if (ret) {
      DLOGE("Failed to get panel id, error = %d", ret);
      return kErrorUndefined;
    }
  }
  panel_id_ = panel_id;
  DLOGI("panel_id 0x%" PRIX64, panel_id_);

  PanelFeaturePropertyInfo demura_info;
  bool double_buffer_codebook_supported = false;
  demura_info.prop_id = kPanelFeatureDemuraDoubleBufferCbFlags;
  demura_info.prop_ptr = reinterpret_cast<uint64_t>(&double_buffer_codebook_supported);
  ret = prop_intf_->GetPanelFeature(&demura_info);
  if (ret) {
    DLOGE("Failed to get panel feature, error = %d", ret);
    return kErrorUndefined;
  }
  double_buffer_codebook_supported_ = double_buffer_codebook_supported;

  // Send Panel ID to parser manager before validating license
  // in case of DemuraTn is enabled with unity config.
  error = SendPanelIdToParserManager();
  if (error) {
    DLOGE("Failed to setup parser manager, error %d", error);
    return error;
  }

  error = ValidateDemuraLicense();
  if (error) {
    DLOGE("Failed to validate license, error %d", error);
  }

  // Setup Demura T0 and Tn
  if (demura_allowed_) {
    error = SetupDemuraT0();
    if (error) {
      DLOGE("Failed to setup demura T0, error %d", error);
      return error;
    }
  }

  if (demura_allowed_ && demuratn_allowed_ && demuratn_factory_) {
    demuratn_user_disabled_ = GetDemuraTnUserCtrl();
    if (!demuratn_user_disabled_) {
      error = SetupDemuraTn();
      if (error != kErrorNone) {
        DLOGW("Failed to setup DemuraTn, Error = %d", error);
      }
    }
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::ValidateDemuraLicense() {
#if defined SDM_UNIT_TESTING || defined TRUSTED_VM
  demura_allowed_ = true;
  demuratn_allowed_ = true;
#else
  if (!feature_license_factory_) {
    DLOGI("Feature license factory is not available");
    return kErrorNone;
  }

  if (!feat_license_intf_) {
    feat_license_intf_ = feature_license_factory_->CreateFeatureLicenseIntf();
    if (!feat_license_intf_) {
      feature_license_factory_ = nullptr;
      DLOGE("Failed to create FeatureLicenseIntf");
      return kErrorUndefined;
    }
    int ret = feat_license_intf_->Init();
    if (ret) {
      DLOGE("Failed to init FeatureLicenseIntf");
      feat_license_intf_.reset();
      feat_license_intf_ = nullptr;
      return kErrorUndefined;
    }
  }

  GenericPayload demura_pl, aa_pl, out_pl;
  DemuraValidatePermissionInput *demura_input = nullptr;
  int ret = demura_pl.CreatePayload<DemuraValidatePermissionInput>(demura_input);
  if (ret) {
    DLOGE("Failed to create the payload. Error:%d", ret);
    return kErrorUndefined;
  }

  bool *allowed = nullptr;
  ret = out_pl.CreatePayload<bool>(allowed);
  if (ret) {
    DLOGE("Failed to create the payload. Error:%d", ret);
    return kErrorUndefined;
  }

  demura_input->id = kDemura;
  demura_input->panel_id = panel_id_;
  ret = feat_license_intf_->ProcessOps(kValidatePermission, demura_pl, &out_pl);
  if (ret) {
    DLOGE("Failed to get the license permission for Demura. Error:%d", ret);
  } else {
    demura_allowed_ = *allowed;
  }

  AntiAgingValidatePermissionInput *aa_input = nullptr;
  ret = aa_pl.CreatePayload<AntiAgingValidatePermissionInput>(aa_input);
  if (ret) {
    DLOGE("Failed to create the payload. Error:%d", ret);
    return kErrorUndefined;
  }

  aa_input->id = kAntiAging;
  ret = feat_license_intf_->ProcessOps(kValidatePermission, aa_pl, &out_pl);
  if (ret) {
    DLOGE("Failed to get the license permission for Anti-aging. Error:%d", ret);
  } else {
    demuratn_allowed_ = *allowed;
  }
#endif

  DLOGI("Demura enable allowed %d, Anti-aging enable allowed %d", demura_allowed_,
        demuratn_allowed_);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupDemuraT0(int current_idx) {
  DisplayError error = kErrorNone;

  error = SetupDemura(current_idx);
  if (error != kErrorNone) {
    DLOGE("Demura failed to initialize on display %d-%d, Error %d", display_id_, display_type_,
          error);
    comp_manager_->FreeDemuraFetchResources(display_id_);
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    if (demura_) {
      SetDemuraIntfStatus(false);
    }
  }

  return error;
}

DisplayError DisplayBuiltIn::SendPanelIdToParserManager() {
  int ret = 0;

  if (!pm_intf_) {
    DLOGE("Invalid Parser Manager intf");
    return kErrorUndefined;
  }

  PanelIdsInfo *panel_ids_info = nullptr;
  GenericPayload in;
  ret = in.CreatePayload<PanelIdsInfo>(panel_ids_info);
  if (ret || !panel_ids_info) {
    DLOGE("Failed to create payload for panel ids, ret %d", ret);
    return kErrorResources;
  }

  panel_ids_info->panel_ids.push_back(panel_id_);
  panel_ids_info->is_primary_display = IsPrimaryDisplayLocked();
  panel_ids_info->double_buffer_codebook_supported = double_buffer_codebook_supported_;
  if ((ret = pm_intf_->SetParameter(kDemuraParserManagerParamPanelIds, in))) {
    DLOGE("Failed to set the panel ids to the parser manager");
    return kErrorResources;
  }

  DLOGI("Successfully set panel ID 0x%" PRIX64 " to parser manager intf, is_primary_display %d", panel_id_,
        panel_ids_info->is_primary_display);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetupDemuraTn() {
  int ret = 0;

  if (!demura_) {
    DLOGI("Demura is not enabled, cannot setup DemuraTn");
    return kErrorNone;
  }

  demuratn_ = demuratn_factory_->CreateDemuraTnCoreUvmIntf(demura_, buffer_allocator_, this);
  if (!demuratn_) {
    DLOGE("Failed to create demuraTnCoreUvmIntf");
    return kErrorUndefined;
  }

  ret = demuratn_->Init();
  if (ret) {
    DLOGE("Failed to init demuraTnCoreUvmIntf, ret %d", ret);
    demuratn_.reset();
    demuratn_ = nullptr;
    return kErrorUndefined;
  }

  // Query the override feature from demuratn_
  GenericPayload payload;
  DemuraFeatureType *override_feature = nullptr;
  ret = payload.CreatePayload<DemuraFeatureType>(override_feature);
  if (ret || !override_feature) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }

  ret = demuratn_->GetParameter(kDemuraTnCoreUvmParamOverrideFeature, &payload);
  if (ret) {
    DLOGW("Failed to get override feature, ret %d", ret);
  } else {
    demuratn_override_feature_ = *override_feature;
    DLOGI("DemuraTn override feature type: %s",
          DemuraFeatureTypeToString(demuratn_override_feature_));
  }

  if (demuratn_override_feature_ == kFeatureDAC) {
    // Clear the DUC multi-config parsers, only keep the T0 base config parser
    ClearDemuraMultiCfgParsers();
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::EnableDemuraTn(bool enable) {
  int ret = 0;
  bool *en = nullptr;
  GenericPayload payload;

  if (!demuratn_) {
    DLOGE("demuratn_ is nullptr");
    return kErrorUndefined;
  }

  if (demuratn_enabled_ == enable)
    return kErrorNone;

  ret = payload.CreatePayload(en);
  if (ret) {
    DLOGE("Failed to create enable payload");
    return kErrorUndefined;
  }
  *en = enable;

  if (enable) {  // make sure init is ready before enabling
    DemuraTnCoreState *init_ready = nullptr;
    GenericPayload ready_pl;
    ret = ready_pl.CreatePayload<DemuraTnCoreState>(init_ready);
    if (ret) {
      DLOGE("failed to create the payload. Error:%d", ret);
      return kErrorUndefined;
    }

    ret = demuratn_->GetParameter(kDemuraTnCoreUvmParamInitReady, &ready_pl);
    if (ret) {
      DLOGE("GetParameter for InitReady failed ret %d", ret);
      return kErrorUndefined;
    }
    if (*init_ready == kDemuraTnCoreNotReady) {
      return kErrorNone;
    } else if (*init_ready == kDemuraTnCoreError) {
      DLOGE("DemuraTn init ready state returns error");
      int rc = demuratn_->Deinit();
      if (rc)
        DLOGE("Failed to deinit DemuraTn ret %d", rc);
      demuratn_.reset();
      demuratn_ = nullptr;
      return kErrorUndefined;
    } else if (*init_ready == kDemuraTnCoreReady) {
      ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamEnable, payload);
      if (ret) {
        DLOGE("SetParameter for enable failed ret %d", ret);
        return kErrorUndefined;
      }
      demuratn_enabled_ = true;
    }
  } else {
    ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamEnable, payload);
    if (ret) {
      DLOGE("SetParameter for enable failed ret %d", ret);
      return kErrorUndefined;
    }
    demuratn_enabled_ = false;
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::RetrieveDemuraTnFiles() {
  int ret = 0;
  GenericPayload payload;

  if (!demuratn_ || !demuratn_enabled_) {
    DLOGE("demuratn_ %pK demuratn_enabled_ %d", demuratn_.get(), demuratn_enabled_);
    return kErrorUndefined;
  }

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamRetrieveFiles, payload);
  if (ret) {
    DLOGE("SetParameter for RetrieveFiles failed ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDisplayStateForDemuraTn(DisplayState state) {
  int ret = 0;
  DisplayState *disp_state = nullptr;
  GenericPayload pl;

  ret = pl.CreatePayload<DisplayState>(disp_state);
  if (ret) {
    DLOGE("failed to create the payload. Error:%d", ret);
    return kErrorUndefined;
  }
  *disp_state = state;

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamDisplayState, pl);
  if (ret) {
    DLOGE("SetParameter for DisplayState failed ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetUpCommit(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  last_panel_mode_ = client_ctx_.hw_panel_info.mode;
  PreCommit(layer_stack);
  if (pending_cycles_for_poms_setup_ && !avoid_vsync_enable_) {
    avoid_vsync_enable_ = true;
    // Need to disable vsync while POMS in progress as it can't be processed by driver.
    SetVsyncStatus(false /*Disable vsync events.*/);
  }

  SetPrivacyRegions();
  return DisplayBase::SetUpCommit(layer_stack);
}

DisplayError DisplayBuiltIn::CommitLocked(LayerStack *layer_stack) {
  DTRACE_SCOPED();
  last_panel_mode_ = client_ctx_.hw_panel_info.mode;
  PreCommit(layer_stack);

  return DisplayBase::CommitLocked(layer_stack);
}

DisplayError DisplayBuiltIn::PostCommit() {
  DisplayError err = kErrorNone;
  DisplayBase::PostCommit();
  // Mutex scope
  {
    lock_guard<recursive_mutex> obj(brightness_lock_);
    if (pending_brightness_) {
      Fence::Wait(retire_fence_);
      err = SetPanelBrightness(cached_brightness_, true);
      pending_brightness_ = false;
    }
  }
  if (err == kErrorNone) {
    HandleDemuraScreenRefresh();
  }

  if (commit_event_enabled_) {
    dpps_info_.DppsNotifyOps(kDppsCommitEvent, &display_type_, sizeof(display_type_));
  }

  deferred_config_.UpdateDeferCount();

  ReconfigureDisplay();

  if (deferred_config_.CanApplyDeferredState()) {
    validated_ = false;
    deferred_config_.Clear();
  }

  clock_gettime(CLOCK_MONOTONIC, &idle_timer_start_);
  int idle_time_ms = disp_layer_stack_->stack_info.common_info.set_idle_time_ms;
  if (idle_time_ms >= 0) {
    dpu_core_mux_->SetIdleTimeoutMs(UINT32(idle_time_ms));
    idle_time_ms_ = idle_time_ms;
  }

  if (switch_to_cmd_) {
    uint32_t pending;
    switch_to_cmd_ = false;
    ControlPartialUpdateLocked(true /* enable */, &pending);
  }

  if (last_panel_mode_ != client_ctx_.hw_panel_info.mode) {
    UpdateDisplayModeParams();
  }

  if (dpps_pu_nofiy_pending_) {
    dpps_pu_nofiy_pending_ = false;
    dpps_pu_lock_.Broadcast();
  }
  dpps_info_.Init(this, client_ctx_.hw_panel_info.panel_name, this, prop_intf_);

  if (demuratn_ && !demuratn_user_disabled_)
    EnableDemuraTn(true);

  HandleQsyncPostCommit();

  handle_idle_timeout_ = false;

  pending_commit_ = false;
  lower_fps_ = false;

  if (pending_cycles_for_poms_setup_ > 0) {
    pending_cycles_for_poms_setup_--;
    avoid_vsync_enable_ = !!pending_cycles_for_poms_setup_;
  }

  return kErrorNone;
}

void DisplayBuiltIn::HandleQsyncPostCommit() {
  if (qsync_mode_ == kQsyncModeOneShot) {
    // Reset qsync mode.
    SetQSyncMode(kQSyncModeNone);
  } else if (qsync_mode_ == kQsyncModeOneShotContinuous) {
    // No action needed.
  } else if (qsync_mode_ == kQSyncModeContinuous) {
    if (!avoid_qsync_mode_change_) {
      needs_avr_update_.reset();
    } else if (needs_avr_update_.any()) {
      validated_ = false;
      event_handler_->Refresh();
    }
  } else if (qsync_mode_ == kQSyncModeNone) {
    needs_avr_update_.reset();
  }

  avoid_qsync_mode_change_ = false;
  SetVsyncStatus(true /*Re-enable vsync.*/);

  bool notify_idle = enable_qsync_idle_ && (active_qsync_mode_ != kQSyncModeNone) &&
                     handle_idle_timeout_;
  if (notify_idle) {
    event_handler_->HandleEvent(kPostIdleTimeout);
  }

  bool qsync_enabled = (active_qsync_mode_ != kQSyncModeNone);
  if (qsync_enabled == qsync_enabled_) {
    return;
  }

  QsyncEventData event_data;
  event_data.enabled = qsync_enabled;
  event_data.refresh_rate = client_ctx_.display_attributes.fps;
  dpu_core_mux_->GetQsyncFps(&event_data.qsync_refresh_rate);
  event_handler_->HandleQsyncState(event_data);

  qsync_enabled_ = qsync_enabled;
}

void DisplayBuiltIn::UpdateDisplayModeParams() {
  if (client_ctx_.hw_panel_info.mode == kModeVideo) {
    uint32_t pending = 0;
    ControlPartialUpdateLocked(false /* enable */, &pending);
  } else if (client_ctx_.hw_panel_info.mode == kModeCommand) {
    // Flush idle timeout value currently set.
    comp_manager_->SetIdleTimeoutMs(display_comp_ctx_, 0, 0);
    switch_to_cmd_ = true;
  }
}

DisplayError DisplayBuiltIn::SetDisplayState(DisplayState state, bool teardown,
                                             shared_ptr<Fence> *release_fence) {
  ClientLock lock(disp_mutex_);
  DisplayError error = kErrorNone;
  HWDisplayMode panel_mode = client_ctx_.hw_panel_info.mode;

  if ((state == kStateOn) && deferred_config_.IsDeferredState()) {
    SetDeferredFpsConfig();
  }

  // Must go in NullCommit
  if (((demura_intended_ && demura_dynamic_enabled_) || abc_enabled_) &&
      comp_manager_->GetDemuraStatusForDisplay(display_id_) && (state == kStateOff)) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    SetDemuraIntfStatus(false, demura_current_idx_);
  }

  if (hfi_path_supported_) {
    if (state == DisplayState::kStateDoze || state == DisplayState::kStateDozeSuspend) {
      // set driver commit path to HFI if entering doze mode
      error = hw_intf_->setDriverCommitPath(DriverCommitPath::kHFI);
    } else if ((state_ == DisplayState::kStateDoze || state_ == DisplayState::kStateDozeSuspend) &&
               (state != DisplayState::kStateDoze && state != DisplayState::kStateDozeSuspend)) {
      // set driver commit path to hwio if exiting doze ode
      error = hw_intf_->setDriverCommitPath(DriverCommitPath::kHWIO);
    }
  }

  if (error) {
    DLOGW("Failed to update driver path when transitioning to state %d", state);
  }

  error = DisplayBase::SetDisplayState(state, teardown, release_fence);
  if (error != kErrorNone) {
    return error;
  }

  if (secure_event_ == kTUITransitionEnd && state == kStateOff) {
    error = SetPanelBrightness(cached_brightness_, true);
    pending_brightness_ = false;
    if (error == kErrorNone) {
      HandleDemuraScreenRefresh();
    }
  }

  if (client_ctx_.hw_panel_info.mode != panel_mode) {
    UpdateDisplayModeParams();
  }

  // Set vsync enable state to false, as driver disables vsync during display power off.
  if (state == kStateOff) {
    vsync_enable_ = false;
    if (qsync_mode_ != kQSyncModeNone) {
      needs_avr_update_.set(kUpdateAVRModeFlag);
    }
  }

  if (pending_power_state_ != kPowerStateNone) {
    event_handler_->Refresh();
  }

  // Must only happen after NullCommit and get applied in next frame
  if (((demura_intended_ && demura_dynamic_enabled_) || abc_enabled_) &&
      !comp_manager_->GetDemuraStatusForDisplay(display_id_) &&
      (state == kStateOn || state == kStateDoze)) {
    comp_manager_->SetDemuraStatusForDisplay(display_id_, true);

    // Enable default idx if demura calib files are reloaded
    if (demura_calib_files_reloaded_) {
      demura_calib_files_reloaded_ = false;
      demura_current_idx_ = kDemuraDefaultIdx;
    }
    SetDemuraIntfStatus(true, demura_current_idx_);
  }

  if (demuratn_ && demuratn_enabled_) {
    SetDisplayStateForDemuraTn(state);
  }

  return kErrorNone;
}

void DisplayBuiltIn::SetIdleTimeoutMs(uint32_t active_ms, uint32_t inactive_ms) {
  ClientLock lock(disp_mutex_);
  comp_manager_->SetIdleTimeoutMs(display_comp_ctx_, active_ms, inactive_ms);
  validated_ = false;
  handle_idle_timeout_ = false;
}

DisplayError DisplayBuiltIn::SetDisplayMode(uint32_t mode) {
  DisplayError error = kErrorNone;

  // Limit scope of mutex to this block
  {
    ClientLock lock(disp_mutex_);
    HWDisplayMode hw_display_mode = static_cast<HWDisplayMode>(mode);
    uint32_t pending = 0;

    if (!active_) {
      DLOGW("Invalid display state = %d. Panel must be on.", state_);
      return kErrorNotSupported;
    }

    if (hw_display_mode != kModeCommand && hw_display_mode != kModeVideo) {
      DLOGW("Invalid panel mode parameters on display %d-%d. Requested = %d",
            display_id_, display_type_, hw_display_mode);
      return kErrorParameters;
    }

    if (hw_display_mode == client_ctx_.hw_panel_info.mode) {
      DLOGW("Same display mode requested on display %d-%d. Current = %d, Requested = %d",
            display_id_, display_type_, client_ctx_.hw_panel_info.mode, hw_display_mode);
      return kErrorNone;
    }

    error = dpu_core_mux_->SetDisplayMode(hw_display_mode);
    if (error != kErrorNone) {
      DLOGW("Retaining current display mode on display %d-%d. Current = %d, Requested = %d",
            display_id_, display_type_, client_ctx_.hw_panel_info.mode, hw_display_mode);
      return error;
    }

    avoid_qsync_mode_change_ = true;
    DisplayBase::ReconfigureDisplay();

    if (mode == kModeVideo) {
      ControlPartialUpdateLocked(false /* enable */, &pending);
      uint32_t active_ms = 0;
      uint32_t inactive_ms = 0;
      Debug::GetIdleTimeoutMs(&active_ms, &inactive_ms);
      comp_manager_->SetIdleTimeoutMs(display_comp_ctx_, active_ms, inactive_ms);
    } else if (mode == kModeCommand) {
      // Flush idle timeout value currently set.
      comp_manager_->SetIdleTimeoutMs(display_comp_ctx_, 0, 0);
      switch_to_cmd_ = true;
    }

    // Assumption: POMS needs setup time in between 30ms to 70ms.
    // Calculate maximum number of cycles needed for POMS setup as per current FPS
    // configuration by considering minimum ~70ms setup time for POMS.
    pending_cycles_for_poms_setup_ = (UINT32(client_ctx_.display_attributes.fps) >> 4) + 1;
  }

  // Request for a new draw cycle. New display mode will get applied on next draw cycle.
  // New idle time will get configured as part of this.
  event_handler_->Refresh();

  return error;
}

DisplayError DisplayBuiltIn::SetPanelBrightness(float brightness, bool apply_immediately,
                                                bool return_error) {
  DisplayError err = kErrorNone;
  {
    lock_guard<recursive_mutex> obj(brightness_lock_);

    if (brightness != -1.0f && !(0.0f <= brightness && brightness <= 1.0f)) {
      DLOGE("Bad brightness value = %f", brightness);
      return kErrorParameters;
    }

    // -1.0f = off, 0.0f = min, 1.0f = max
    float level_remainder = 0.0f;
    int level = 0;
    if (brightness == -1.0f) {
      level = 0;
    } else {
      // Node only supports int level, so store the float remainder for accurate GetPanelBrightness
      float max = client_ctx_.hw_panel_info.panel_max_brightness;
      float min = client_ctx_.hw_panel_info.panel_min_brightness;
      if (min >= max) {
        DLOGE("Minimum brightness is greater than or equal to maximum brightness");
        return kErrorDriverData;
      }
      float t = (brightness * (max - min)) + min;
      level = static_cast<int>(t);
      level_remainder = t - level;
    }

    abc_brightness_level_ = level;
    err = dpu_core_mux_->SetPanelBrightness(level, apply_immediately);
    if (err == kErrorNone) {
      level_remainder_ = level_remainder;
      pending_brightness_ = false;
      comp_manager_->SetBacklightLevel(display_comp_ctx_, level);
      DLOGI_IF(kTagDisplay, "Setting brightness to level %d (%f percent)", level, brightness * 100);
    } else if (err == kErrorDeferred) {
      // TODO(user): I8508d64a55c3b30239c6ed2886df391407d22f25 causes mismatch between perceived
      // power state and actual panel power state. Requires a rework. Below check will set up
      // deferment of brightness operation if DAL reports defer use case.
      cached_brightness_ = brightness;
      pending_brightness_ = true;
      return return_error ? kErrorDeferred : kErrorNone;
    }
  }

  if (!return_error && err == kErrorNone) {
    HandleDemuraScreenRefresh();
  }

  return err;
}

DisplayError DisplayBuiltIn::SetBppMode(uint32_t bpp) {
  {
    ClientLock lock(disp_mutex_);

    DisplayError error = hw_intf_->SetBppMode(bpp);
    if (error != kErrorNone) {
      DLOGW("Retaining current panel bpp mode on display %d-%d. Requested = 0x%x",
            display_id_, display_type_, bpp);
      return error;
    }
    DisplayBase::ReconfigureDisplay();
    shared_ptr<Fence> release_fence = nullptr;
    SetDisplayState(kStateOff, 0, &release_fence);
    sleep(1);
    SetDisplayState(kStateOn, 0, &release_fence);
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::GetRefreshRateRange(uint32_t *min_refresh_rate,
                                                 uint32_t *max_refresh_rate) {
  ClientLock lock(disp_mutex_);
  DisplayError error = kErrorNone;

  if (client_ctx_.hw_panel_info.min_fps && client_ctx_.hw_panel_info.max_fps) {
    *min_refresh_rate = client_ctx_.hw_panel_info.min_fps;
    *max_refresh_rate = client_ctx_.hw_panel_info.max_fps;
  } else {
    error = DisplayBase::GetRefreshRateRange(min_refresh_rate, max_refresh_rate);
  }

  return error;
}

DisplayError DisplayBuiltIn::SetRefreshRate(uint32_t refresh_rate, bool final_rate,
                                            bool idle_screen) {
  ClientLock lock(disp_mutex_);

  if (!active_ || !client_ctx_.hw_panel_info.dynamic_fps || qsync_mode_ != kQSyncModeNone ||
      disable_dyn_fps_) {
    return kErrorNotSupported;
  }

  if (refresh_rate < client_ctx_.hw_panel_info.min_fps ||
      refresh_rate > client_ctx_.hw_panel_info.max_fps) {
    DLOGE("Invalid Fps = %d request", refresh_rate);
    return kErrorParameters;
  }

  if (CanLowerFps(idle_screen) && !final_rate && !enable_qsync_idle_) {
    refresh_rate = client_ctx_.hw_panel_info.min_fps;
  }

  if (current_refresh_rate_ != refresh_rate) {
    DisplayError error = dpu_core_mux_->SetRefreshRate(refresh_rate);
    if (error != kErrorNone) {
      // Attempt to update refresh rate can fail if rf interfenence is detected.
      // Just drop min fps settting for now.
      handle_idle_timeout_ = false;
      return error;
    }

    error = comp_manager_->CheckEnforceSplit(display_comp_ctx_, refresh_rate);
    if (error != kErrorNone) {
      return error;
    }
  }

  // Set safe mode upon success.
  if (enhance_idle_time_ && handle_idle_timeout_ &&
      (refresh_rate == client_ctx_.hw_panel_info.min_fps)) {
    comp_manager_->ProcessIdleTimeout(display_comp_ctx_);
  }

  // On success, set current refresh rate to new refresh rate
  current_refresh_rate_ = refresh_rate;
  deferred_config_.MarkDirty();

  return ReconfigureDisplay();
}

bool DisplayBuiltIn::CanLowerFps(bool idle_screen) {
  if (!enhance_idle_time_) {
    return handle_idle_timeout_;
  }

  if (!handle_idle_timeout_ || !idle_screen) {
    return false;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t elapsed_time_ms = GetTimeInMs(now) - GetTimeInMs(idle_timer_start_);
  bool can_lower = elapsed_time_ms >= UINT32(idle_time_ms_);
  DLOGV_IF(kTagDisplay, "lower fps: %d", can_lower);

  return can_lower;
}

DisplayError DisplayBuiltIn::VSync(int64_t timestamp) {
  DTRACE_SCOPED();
  bool qsync_enabled = enable_qsync_idle_ && (active_qsync_mode_ != kQSyncModeNone);
  // Client isn't aware of underlying qsync mode.
  // Disable vsync propagation as long as qsync is enabled.
  bool propagate_vsync = vsync_enable_ && !drop_hw_vsync_ && !qsync_enabled;
  if (!propagate_vsync) {
    // Re enable when display updates.
    SetVsyncStatus(false /*Disable vsync events.*/);
    return kErrorNone;
  }

  DisplayEventVSync vsync;
  vsync.timestamp = timestamp;
  event_handler_->VSync(vsync);

  return kErrorNone;
}

void DisplayBuiltIn::SetVsyncStatus(bool enable) {
  string trace_name = enable ? "enable" : "disable";
  DTRACE_BEGIN(trace_name.c_str());
  if (enable) {
    // Enable if vsync is still enabled.
    if (vsync_enable_ && !avoid_vsync_enable_) {
      master_hw_events_intf_->SetEventState(HWEvent::VSYNC, vsync_enable_);
    }
  } else {
    master_hw_events_intf_->SetEventState(HWEvent::VSYNC, false);
  }
  DTRACE_END();
}

bool DisplayBuiltIn::IdleTimeout() {
  DTRACE_SCOPED();
  if (pending_cycles_for_poms_setup_ > 0) {
    pending_cycles_for_poms_setup_ = 0;
    avoid_vsync_enable_ = false;
  }

  if ((state_ == kStateOff) || avr_step_enabled_) {
    return false;
  }

  if (pending_commit_) {
    return false;
  }

  handle_idle_timeout_ = true;
  if (!enhance_idle_time_) {
    comp_manager_->ProcessIdleTimeout(display_comp_ctx_);
  }

  validated_ = false;
  event_handler_->Refresh();
  return true;
}

void DisplayBuiltIn::TriggerIdleTimeout() {
  DTRACE_SCOPED();
  if (handle_idle_timeout_ || !is_mirror_mode_active_) {
    return;
  }
  ClientLock lock(disp_mutex_);
  trigger_idle_timeout_ = true;
  DLOGI_IF(kTagDisplay, "Unlock Commit Thread to perform idle-timeout ...");
  lock.NotifyWorker();
}

void DisplayBuiltIn::PingPongTimeout() {
  ClientLock lock(disp_mutex_);
  dpu_core_mux_->DumpDebugData();
}

void DisplayBuiltIn::IdlePowerCollapse() {
  if ((client_ctx_.hw_panel_info.mode == kModeCommand) || client_ctx_.hw_panel_info.vhm_support) {
    ClientLock lock(disp_mutex_);
    validated_ = false;
    comp_manager_->ProcessIdlePowerCollapse(display_comp_ctx_);
    event_handler_->HandleEvent(kIdleTimeout);
    if (pending_cycles_for_poms_setup_ > 0) {
      pending_cycles_for_poms_setup_ = 0;
      avoid_vsync_enable_ = false;
    }
  }
}

DisplayError DisplayBuiltIn::ClearLUTs() {
  ClientLock lock(disp_mutex_);
  validated_ = false;
  comp_manager_->ProcessIdlePowerCollapse(display_comp_ctx_);
  return kErrorNone;
}

void DisplayBuiltIn::MMRMEvent(uint32_t clk) {
  DTRACE_SCOPED();
  DisplayBase::MMRMEvent(clk);
}

void DisplayBuiltIn::PanelDead() {
  {
    ClientLock lock(disp_mutex_);
    reset_panel_ = true;
    validated_ = false;
  }
  event_handler_->HandleEvent(kPanelDeadEvent);
  event_handler_->Refresh();
}

// HWEventHandler overload, not DisplayBase
void DisplayBuiltIn::HwRecovery(const HWRecoveryEvent sdm_event_code) {
  DisplayBase::HwRecovery(sdm_event_code);
}

void DisplayBuiltIn::Histogram(int histogram_fd, uint32_t blob_id) {
  event_handler_->HistogramEvent(histogram_fd, blob_id);
}

void DisplayBuiltIn::HandleBacklightEvent(float brightness_level) {
  DLOGI("backlight event occurred %f ipc_intf %p", brightness_level, ipc_intf_.get());
  if (ipc_intf_) {
    GenericPayload in;
    IPCBacklightParams *backlight_params = nullptr;
    int ret = in.CreatePayload<IPCBacklightParams>(backlight_params);
    if (ret) {
      DLOGW("failed to create the payload. Error:%d", ret);
      return;
    }
    float brightness = 0.0f;
    if (GetPanelBrightnessFromLevel(brightness_level, &brightness) != kErrorNone) {
      return;
    }
    backlight_params->brightness = brightness;
    backlight_params->is_primary = IsPrimaryDisplayLocked();
    if ((ret = ipc_intf_->SetParameter(kIpcParamBacklight, in))) {
      DLOGW("Failed to set backlight, error = %d", ret);
    }
    lock_guard<recursive_mutex> obj(brightness_lock_);
    cached_brightness_ = brightness;
    pending_brightness_ = true;
  }
}

DisplayError DisplayBuiltIn::GetPanelBrightness(float *brightness) {
  lock_guard<recursive_mutex> obj(brightness_lock_);

  DisplayError err = kErrorNone;
  int level = 0;
  if ((err = dpu_core_mux_->GetPanelBrightness(&level)) != kErrorNone) {
    return err;
  }
  return GetPanelBrightnessFromLevel(level, brightness);
}

DisplayError DisplayBuiltIn::GetPanelBrightnessFromLevel(float level, float *brightness) {
  // -1.0f = off, 0.0f = min, 1.0f = max
  float max = client_ctx_.hw_panel_info.panel_max_brightness;
  float min = client_ctx_.hw_panel_info.panel_min_brightness;
  if (level == 0) {
    *brightness = -1.0f;
  } else if ((max > min) && (min <= level && level <= max)) {
    *brightness = (static_cast<float>(level) + level_remainder_ - min) / (max - min);
  } else {
    min >= max ? DLOGE("Minimum brightness is greater than or equal to maximum brightness") :
                 DLOGE("Invalid brightness level %f", level);
    return kErrorDriverData;
  }

  DLOGI_IF(kTagDisplay, "Received level %f (%f percent)", level, *brightness * 100);

  return kErrorNone;
}

DisplayError DisplayBuiltIn::GetPanelBrightnessLevel(int *level) {
  lock_guard<recursive_mutex> obj(brightness_lock_);

  if (!level) {
    DLOGE("Invalid input pointer is null");
    return kErrorParameters;
  }

  DisplayError err = dpu_core_mux_->GetPanelBrightness(level);
  if (err != kErrorNone) {
    DLOGE("Failed to get panel brightness, err %d", err);
    return err;
  }

  DLOGI_IF(kTagDisplay, "Current panel level %d", *level);
  return err;
}

DisplayError DisplayBuiltIn::GetPanelMaxBrightness(uint32_t *max_brightness_level) {
  lock_guard<recursive_mutex> obj(brightness_lock_);

  if (!max_brightness_level) {
    DLOGE("Invalid input pointer is null");
    return kErrorParameters;
  }

  *max_brightness_level = static_cast<uint32_t>(client_ctx_.hw_panel_info.panel_max_brightness);

  DLOGI_IF(kTagDisplay, "Get panel max_brightness_level %u", *max_brightness_level);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::ControlPartialUpdate(bool enable, uint32_t *pending) {
  ClientLock lock(disp_mutex_);
  return ControlPartialUpdateLocked(enable, pending);
}

DisplayError DisplayBuiltIn::ControlPartialUpdateLocked(bool enable, uint32_t *pending) {
  if (!pending) {
    return kErrorParameters;
  }

  if (dpps_info_.disable_pu_ && enable) {
    // Nothing to be done.
    DLOGI("partial update is disabled by DPPS for display %d-%d", display_id_, display_type_);
    return kErrorNotSupported;
  }

  *pending = 0;
  if (enable == partial_update_control_) {
    DLOGI("Same state transition is requested.");
    return kErrorNone;
  }
  validated_ = false;
  partial_update_control_ = enable;

  if (!enable) {
    // If the request is to turn off feature, new draw call is required to have
    // the new setting into effect.
    *pending = 1;
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::DisablePartialUpdateOneFrame() {
  ClientLock lock(disp_mutex_);
  disable_pu_one_frame_ = true;
  validated_ = false;

  return kErrorNone;
}

DisplayError DisplayBuiltIn::DisablePartialUpdateOneFrameInternal() {
  disable_pu_one_frame_ = true;
  validated_ = false;

  return kErrorNone;
}

DisplayError DisplayBuiltIn::DppsProcessOps(enum DppsOps op, void *payload, size_t size) {
  DisplayError error = kErrorNone;
  uint32_t pending;
  bool enable = false;
  DppsDisplayInfo *info;

  switch (op) {
    case kDppsSetFeature:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      {
        ClientLock lock(disp_mutex_);
        error = SetDppsFeatureLocked(payload, size);
      }
      break;
    case kDppsGetFeatureInfo:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      error = dpu_core_mux_->GetDppsFeatureInfo(payload, size);
      break;
    case kDppsScreenRefresh:
      HandleSelfRefresh();
      break;
    case kDppsPartialUpdate: {
      int ret;
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      enable = *(reinterpret_cast<bool *>(payload));
      dpps_info_.disable_pu_ = !enable;
      ControlPartialUpdate(enable, &pending);
      event_handler_->Refresh();
      {
        ClientLock lock(disp_mutex_);
        validated_ = false;
        dpps_pu_nofiy_pending_ = true;
      }
      ret = dpps_pu_lock_.WaitFinite(kPuTimeOutMs);
      if (ret) {
        DLOGW("failed to %s partial update ret %d", ((enable) ? "enable" : "disable"), ret);
        error = kErrorTimeOut;
      }
      break;
    }
    case kDppsRequestCommit:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      {
        ClientLock lock(disp_mutex_);
        commit_event_enabled_ = *(reinterpret_cast<bool *>(payload));
      }
      break;
    case kDppsGetDisplayInfo:
      if (!payload) {
        DLOGE("Invalid payload parameter for op %d", op);
        error = kErrorParameters;
        break;
      }
      info = reinterpret_cast<DppsDisplayInfo *>(payload);
      info->width = client_ctx_.display_attributes.x_pixels;
      info->height = client_ctx_.display_attributes.y_pixels;
      info->is_primary = IsPrimaryDisplayLocked();
      info->display_id = display_id_;
      info->display_type = display_type_;
      info->fps = enable_dpps_dyn_fps_ ? client_ctx_.display_attributes.fps : 0;

      error = dpu_core_mux_->GetPanelBrightnessBasePath(&(info->brightness_base_path));
      if (error != kErrorNone) {
        DLOGE("Failed to get brightness base path %d", error);
      }
      break;
    case kDppsSetPccConfig:
      error = color_mgr_->ColorMgrSetLtmPccConfig(payload, size);
      if (error != kErrorNone) {
        DLOGE("Failed to set PCC config to ColorManagerProxy, error %d", error);
      } else {
        ClientLock lock(disp_mutex_);
        validated_ = false;
        DisablePartialUpdateOneFrameInternal();
      }
      break;
    default:
      DLOGE("Invalid input op %d", op);
      error = kErrorParameters;
      break;
  }
  return error;
}

DisplayError DisplayBuiltIn::SetDisplayDppsAdROI(void *payload) {
  ClientLock lock(disp_mutex_);
  DisplayError err = kErrorNone;

  err = dpu_core_mux_->SetDisplayDppsAdROI(payload);
  if (err != kErrorNone)
    DLOGE("Failed to set ad roi config, err %d", err);

  return err;
}

DisplayError DisplayBuiltIn::SetFrameTriggerMode(FrameTriggerMode mode) {
  ClientLock lock(disp_mutex_);
  validated_ = false;
  trigger_mode_debug_ = mode;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::GetStcColorModes(snapdragoncolor::ColorModeList *mode_list) {
  ClientLock lock(disp_mutex_);
  if (!mode_list) {
    return kErrorParameters;
  }

  if (!color_mgr_) {
    return kErrorNotSupported;
  }

  mode_list->list = stc_color_modes_.list;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetStcColorMode(const snapdragoncolor::ColorMode &color_mode) {
  ClientLock lock(disp_mutex_);
  if (!color_mgr_) {
    return kErrorNotSupported;
  }
  DisplayError ret = kErrorNone;
  PrimariesTransfer blend_space = {};
  blend_space = GetBlendSpaceFromStcColorMode(color_mode);
  ret = comp_manager_->SetBlendSpace(display_comp_ctx_, blend_space);
  if (ret != kErrorNone) {
    DLOGE("SetBlendSpace failed, ret = %d on display %d-%d", ret, display_id_, display_type_);
  }

  ret = dpu_core_mux_->SetBlendSpace(blend_space);
  if (ret != kErrorNone) {
    DLOGE("Failed to pass blend space, ret = %d on display %d-%d", ret, display_id_,
          display_type_);
  }

  ret = color_mgr_->ColorMgrSetStcMode(color_mode);
  if (ret != kErrorNone) {
    DLOGE("Failed to set stc color mode, ret = %d on display %d-%d", ret,
          display_id_, display_type_);
    return ret;
  }

  current_color_mode_ = color_mode;

  DynamicRangeType dynamic_range = kSdrType;
  if (std::find(color_mode.hw_assets.begin(), color_mode.hw_assets.end(),
                snapdragoncolor::kPbHdrBlob) != color_mode.hw_assets.end()) {
    dynamic_range = kHdrType;
  }
  if ((color_mode.gamut == ColorPrimaries_BT2020 && color_mode.gamma == Transfer_SMPTE_ST2084) ||
      (color_mode.gamut == ColorPrimaries_BT2020 && color_mode.gamma == Transfer_HLG)) {
    dynamic_range = kHdrType;
  }
  comp_manager_->ControlDpps(dynamic_range != kHdrType);

  return ret;
}

DisplayError DisplayBuiltIn::NotifyDisplayCalibrationMode(bool in_calibration) {
  ClientLock lock(disp_mutex_);
  if (!color_mgr_) {
    return kErrorNotSupported;
  }
  DisplayError ret = kErrorNone;
  ret = color_mgr_->NotifyDisplayCalibrationMode(in_calibration);
  if (ret != kErrorNone) {
    DLOGE("Failed to notify QDCM Mode status, ret = %d state = %d", ret, in_calibration);
  }

  return ret;
}

bool DisplayBuiltIn::IsAnamorphicFoveationEnabled(LayerStack *layer_stack) {
  if (!xr_variant_) {
    return false;
  }

  const std::vector<Layer *> &layers = layer_stack->layers;
  for (uint32_t i = 0; i < layers.size(); i++) {
    QtiAnamorphicMetadata anamorphic_md = layers[i]->input_buffer.anamorphicMetadata;
    if (anamorphic_md.leftEyeDataValid || anamorphic_md.rightEyeDataValid) {
      return true;
    }
  }

  return false;
}

std::string DisplayBuiltIn::Dump() {
  ClientLock lock(disp_mutex_);
  uint32_t active_index = 0;
  uint32_t num_modes = 0;
  std::ostringstream os;
  char capabilities[16];
  CacVersion cac_version = GetCacVerion();
  HWPanelInfo hw_panel_info = client_ctx_.hw_panel_info;
  HWDisplayAttributes display_attributes = client_ctx_.display_attributes;
  HWMixerAttributes mixer_attributes = client_ctx_.mixer_attributes;

  dpu_core_mux_->GetNumDisplayAttributes(&num_modes);
  dpu_core_mux_->GetActiveConfig(&active_index);

  os << "device type:" << display_type_;
  os << " DrawMethod: " << draw_method_;
  os << "\nstate: " << state_ << " vsync on: " << vsync_enable_
     << " max. mixer stages: " << max_mixer_stages_;
  if (disp_layer_stack_->stack_info.noise_layer_info.enable) {
    os << "\nNoise z-orders: [" << disp_layer_stack_->stack_info.noise_layer_info.zpos_noise
       << "," << disp_layer_stack_->stack_info.noise_layer_info.zpos_attn << "]";
  }
  os << "\nnum configs: " << num_modes << " active config index: " << active_index;
  os << "\nDisplay Attributes:";
  os << "\n Mode:" << (hw_panel_info.mode == kModeVideo ? "Video" : "Command");
  os << std::boolalpha;
  os << " Primary:" << hw_panel_info.is_primary_panel;
  os << " DynFPS:" << hw_panel_info.dynamic_fps;
  os << "\n HDR Panel:" << hw_panel_info.hdr_enabled;
  os << " QSync:" << hw_panel_info.qsync_support;
  os << " DynBitclk:" << hw_panel_info.dyn_bitclk_support;
  os << "\n Left Split:" << hw_panel_info.split_info.left_split
     << " Right Split:" << hw_panel_info.split_info.right_split;
  os << "\n PartialUpdate:" << hw_panel_info.partial_update;
  if (hw_panel_info.partial_update) {
    os << "\n ROI Min w:" << hw_panel_info.min_roi_width;
    os << " Min h:" << hw_panel_info.min_roi_height;
    os << " NeedsMerge: " << hw_panel_info.needs_roi_merge;
    os << " Alignment: l:" << hw_panel_info.left_align << " w:" << hw_panel_info.width_align;
    os << " t:" << hw_panel_info.top_align << " b:" << hw_panel_info.height_align;
  }
  os << "\n FPS min:" << hw_panel_info.min_fps << " max:" << hw_panel_info.max_fps
     << " cur:" << display_attributes.fps;
  os << " TransferTime: " << hw_panel_info.transfer_time_us << "us";
  os << " Min TransferTime: " << hw_panel_info.transfer_time_us_min << "us";
  os << " Max TransferTime: " << hw_panel_info.transfer_time_us_max << "us";
  os << " AllowedModeSwitch: " << hw_panel_info.allowed_mode_switch;
  os << " PanelModeCaps: ";
  snprintf(capabilities, sizeof(capabilities), "0x%x", hw_panel_info.panel_mode_caps);
  os << capabilities;
  os << " MaxBrightness:" << hw_panel_info.panel_max_brightness;
  os << "\n Display WxH: " << display_attributes.x_pixels << "x" << display_attributes.y_pixels;
  os << " MixerWxH: " << mixer_attributes.width << "x" << mixer_attributes.height;
  os << " DPI: " << display_attributes.x_dpi << "x" << display_attributes.y_dpi;
  os << " LM_Split: " << display_attributes.is_device_split;
  os << "\n vsync_period " << display_attributes.vsync_period_ns;
  os << " v_back_porch: " << display_attributes.v_back_porch;
  os << " v_front_porch: " << display_attributes.v_front_porch;
  os << " v_pulse_width: " << display_attributes.v_pulse_width;
  os << "\n v_total: " << display_attributes.v_total;
  os << " h_total: " << display_attributes.h_total;
  os << " clk: " << display_attributes.clock_khz;
  os << " Topology: " << display_attributes.topology;
  os << " Qsync mode: " << active_qsync_mode_;
  os << " RGBA Split Mode enable: " << rgba_split_enable_;
  os << " CAC enabled: " << disp_layer_stack_->stack_info.enable_cac;
  os << (disp_layer_stack_->stack_info.enable_cac
             ? (cac_version == kCacVersionLoopback) ? " (CACLoopback)" : " (CACV2)"
             : "");
  os << "\n Foveation enabled: " << disp_layer_stack_->stack_info.enable_anamorphic_fov;
  os << (disp_layer_stack_->stack_info.enable_anamorphic_fov ? " (Anamorphic Foveation)" : "");
  os << std::noboolalpha;

  DynamicRangeType curr_dynamic_range = kSdrType;
  if (std::find(current_color_mode_.hw_assets.begin(), current_color_mode_.hw_assets.end(),
                snapdragoncolor::kPbHdrBlob) != current_color_mode_.hw_assets.end()) {
    curr_dynamic_range = kHdrType;
  }
  os << "\nCurrent Color Mode: gamut " << current_color_mode_.gamut << " gamma "
     << current_color_mode_.gamma << " intent " << current_color_mode_.intent << " Dynamice_range"
     << (curr_dynamic_range == kSdrType ? " SDR" : " HDR");

  for (int j = 0; j < core_count_; j++) {
    uint32_t core_id = hw_resource_info_[j].core_id;
    uint32_t num_hw_layers = UINT32(disp_layer_stack_->info.at(core_id).hw_layers.size());

    if (num_hw_layers == 0) {
      os << "\nNo hardware layers programmed";
      return os.str();
    }

    os << "\n\n Table for DPU - " << j << "\n";
    if (cwb_active_) {
      os << "\n Output buffer res: " << cwb_output_buf_.width << "x" << cwb_output_buf_.height
         << " format: " << GetFormatString(cwb_output_buf_.format);
    }

    HWLayersInfo &layer_info = disp_layer_stack_->info.at(core_id);
    for (uint32_t i = 0; i < layer_info.left_frame_roi.size(); i++) {
      LayerRect &l_roi = layer_info.left_frame_roi.at(i);
      LayerRect &r_roi = layer_info.right_frame_roi.at(i);

      os << "\nROI(LTRB)#" << i << " LEFT(" << INT(l_roi.left) << " " << INT(l_roi.top) << " " <<
        INT(l_roi.right) << " " << INT(l_roi.bottom) << ")";
      if (IsValid(r_roi)) {
      os << " RIGHT(" << INT(r_roi.left) << " " << INT(r_roi.top) << " " << INT(r_roi.right) << " "
        << INT(r_roi.bottom) << ")";
      }
    }

    LayerRect &fb_roi = disp_layer_stack_->stack_info.partial_fb_roi;
    if (IsValid(fb_roi)) {
      os << "\nPartial FB ROI(LTRB):(" << INT(fb_roi.left) << " " << INT(fb_roi.top) << " " <<
        INT(fb_roi.right) << " " << INT(fb_roi.bottom) << ")";
    }

    AppendRCMaskData(os);

    const char *header  = "\n| Idx |   Comp Type   |   Split   | Pipe |    W x H    |          Format          |  Src Rect (L T R B) |  Dst Rect (L T R B) |  Z | Pipe Flags | Deci(HxV) | CS | Rng | Tr |";  //NOLINT
    const char *newline = "\n|-----|---------------|-----------|------|-------------|--------------------------|---------------------|---------------------|----|------------|-----------|----|-----|----|";  //NOLINT
    const char *format  = "\n| %3s | %13s | %9s | %4d | %4d x %4d | %24s | %4d %4d %4d %4d | %4d %4d %4d %4d | %2s | %10s | %9s | %2s | %3s | %2s |";  //NOLINT

    os << "\n";
    os << newline;
    os << header;
    os << newline;

    for (uint32_t i = 0; i < num_hw_layers; i++) {
      uint32_t layer_index = disp_layer_stack_->info.at(core_id).index.at(i);
      // hw-layer from hw layers info
      Layer &hw_layer = disp_layer_stack_->info.at(core_id).hw_layers.at(i);
      LayerBuffer *input_buffer = &hw_layer.input_buffer;
      HWLayerConfig &layer_config = disp_layer_stack_->info.at(core_id).config[i];
      HWRotatorSession &hw_rotator_session = layer_config.hw_rotator_session;

      const char *comp_type = GetCompositionName(hw_layer.composition);
      const char *buffer_format = GetFormatString(input_buffer->format);
      const char *pipe_split[4] = {"Pipe-1", "Pipe-2", "Pipe-3", "Pipe-4"};
      const char *rot_pipe[4] = {"Rot-inl-1", "Rot-inl-2", "Rot-inl-3", "Rot-inl-4"};
      char idx[8];

      snprintf(idx, sizeof(idx), "%d", layer_index);

      for (uint32_t count = 0; count < hw_rotator_session.hw_block_count; count++) {
        char row[1024];
        HWRotateInfo &rotate = hw_rotator_session.hw_rotate_info[count];
        LayerRect &src_roi = rotate.src_roi;
        LayerRect &dst_roi = rotate.dst_roi;
        char rot[12] = { 0 };

        snprintf(rot, sizeof(rot), "Rot-%s-%d", layer_config.use_inline_rot ?
                 "inl" : "off", count + 1);

        snprintf(row, sizeof(row), format, idx, comp_type, rot,
                 0, input_buffer->width, input_buffer->height, buffer_format,
                 INT(src_roi.left), INT(src_roi.top), INT(src_roi.right), INT(src_roi.bottom),
                 INT(dst_roi.left), INT(dst_roi.top), INT(dst_roi.right), INT(dst_roi.bottom),
                 "-", "-    ", "-    ", "-", "-", "-");
        os << row;
        // print the below only once per layer block, fill with spaces for rest.
        idx[0] = 0;
        comp_type = "";
      }

      if (hw_rotator_session.hw_block_count > 0) {
        input_buffer = &hw_rotator_session.output_buffer;
        buffer_format = GetFormatString(input_buffer->format);
      }

      if (layer_config.use_solidfill_stage) {
        LayerRect src_roi = layer_config.hw_solidfill_stage.roi;
        const char *decimation = "";
        char flags[16] = { 0 };
        char z_order[8] = { 0 };
        const char *color_primary = "";
        const char *range = "";
        const char *transfer = "";
        char row[1024] = { 0 };

        snprintf(z_order, sizeof(z_order), "%d", layer_config.hw_solidfill_stage.z_order);
        snprintf(flags, sizeof(flags), "0x%08x", hw_layer.flags.flags);
        snprintf(row, sizeof(row), format, idx, comp_type, pipe_split[0],
                 0, INT(src_roi.right), INT(src_roi.bottom),
                 buffer_format, INT(src_roi.left), INT(src_roi.top),
                 INT(src_roi.right), INT(src_roi.bottom), INT(src_roi.left),
                 INT(src_roi.top), INT(src_roi.right), INT(src_roi.bottom),
                 z_order, flags, decimation, color_primary, range, transfer);
        os << row;
        continue;
      }

      for (auto count = 0; count < layer_config.hw_pipes.size(); count++) {
        char decimation[16] = { 0 };
        char flags[16] = { 0 };
        char z_order[8] = { 0 };
        char color_primary[8] = { 0 };
        char range[8] = { 0 };
        char transfer[8] = { 0 };
        bool rot = layer_config.use_inline_rot;

        HWPipeInfo &pipe = layer_config.hw_pipes.at(count);

        if (!pipe.valid) {
          continue;
        }

        LayerRect src_roi = pipe.src_roi;
        LayerRect &dst_roi = pipe.dst_roi;

        snprintf(z_order, sizeof(z_order), "%d", pipe.z_order);
        snprintf(flags, sizeof(flags), "0x%08x", pipe.flags);
        snprintf(decimation, sizeof(decimation), "%3d x %3d", pipe.horizontal_decimation,
                 pipe.vertical_decimation);
        Dataspace &color_metadata = hw_layer.input_buffer.dataspace;
        snprintf(color_primary, sizeof(color_primary), "%d", color_metadata.colorPrimaries);
        snprintf(range, sizeof(range), "%d", color_metadata.range);
        snprintf(transfer, sizeof(transfer), "%d", color_metadata.transfer);

        char row[1024];
        snprintf(row, sizeof(row), format, idx, comp_type, rot ? rot_pipe[count] :
                 pipe_split[count], pipe.pipe_id, input_buffer->width, input_buffer->height,
                 buffer_format, INT(src_roi.left), INT(src_roi.top),
                 INT(src_roi.right), INT(src_roi.bottom), INT(dst_roi.left),
                 INT(dst_roi.top), INT(dst_roi.right), INT(dst_roi.bottom),
                 z_order, flags, decimation, color_primary, range, transfer);

        os << row;
        // print the below only once per layer block, fill with spaces for rest.
        idx[0] = 0;
        comp_type = "";
      }
    }
    os << comp_manager_->Dump(display_comp_ctx_);
    os << newline << "\n";
  }
  return os.str();
}


DppsInterface* DppsInfo::dpps_intf_ = NULL;
std::vector<int32_t> DppsInfo::display_id_ = {};

void DppsInfo::Init(DppsPropIntf *intf, const std::string &panel_name,
                    DisplayInterface *display_intf, PanelFeaturePropertyIntf *prop_intf) {
  std::lock_guard<std::mutex> guard(lock_);
  int error = 0;
  int disable_dpps_features = 0;

  if (!intf || !display_intf || !prop_intf) {
    DLOGE("Invalid intf %pK display_intf %pK prop_intf %pK", intf, display_intf, prop_intf);
    return;
  }

  DppsDisplayInfo info_payload = {};
  DisplayError ret = intf->DppsProcessOps(kDppsGetDisplayInfo, &info_payload, sizeof(info_payload));
  if (ret != kErrorNone) {
    DLOGE("Get display information failed, ret %d", ret);
    return;
  }

  if (std::find(display_id_.begin(), display_id_.end(), info_payload.display_id)
    != display_id_.end()) {
    return;
  }
  DLOGI("Ready to register display %d-%d ", info_payload.display_id,
        info_payload.display_type);

  Debug::Get()->GetProperty(DISABLE_DPPS_FEATURES, &disable_dpps_features);
  if (disable_dpps_features) {
    if (!dpps_intf_) {
      dpps_intf_ = new DppsDummyImpl();
    }
  }

  if (!dpps_intf_) {
    if (!dpps_impl_lib_.Open(kDppsLib_)) {
      DLOGW("Failed to load Dpps lib %s", kDppsLib_);
      goto exit;
    }

    if (!dpps_impl_lib_.Sym("GetDppsInterface", reinterpret_cast<void **>(&GetDppsInterface))) {
      DLOGE("GetDppsInterface not found!, err %s", dlerror());
      goto exit;
    }

    dpps_intf_ = GetDppsInterface();
    if (!dpps_intf_) {
      DLOGE("Failed to get Dpps Interface!");
      goto exit;
    }
  }
  error = dpps_intf_->Init(intf, panel_name, display_intf, prop_intf);
  if (error) {
    DLOGE("DPPS Interface init failure with err %d", error);
    goto exit;
  }

  display_id_.push_back(info_payload.display_id);
  DLOGI("Registered display %d-%d successfully", info_payload.display_id,
        info_payload.display_type);
  return;

exit:
  Deinit_nolock();
  if (!dpps_intf_) {
    dpps_intf_ = new DppsDummyImpl();
    display_id_.push_back(info_payload.display_id);
  }
}

void DppsInfo::Deinit_nolock() {
  if (dpps_intf_) {
    dpps_intf_->Deinit();
    dpps_intf_ = NULL;
  }
  dpps_impl_lib_.~DynLib();
  DLOGI("Dpps info deinit done");
}

void DppsInfo::Deinit() {
  std::lock_guard<std::mutex> guard(lock_);
  Deinit_nolock();
}

void DppsInfo::DppsNotifyOps(enum DppsNotifyOps op, void *payload, size_t size) {
  int ret = 0;
  if (!dpps_intf_) {
    DLOGW("Dpps intf nullptr");
    return;
  }
  ret = dpps_intf_->DppsNotifyOps(op, payload, size);
  if (ret)
    DLOGE("DppsNotifyOps op %d error %d", op, ret);
}

DisplayError DisplayBuiltIn::GetQSyncMode(QSyncMode *qsync_mode) {
  *qsync_mode = active_qsync_mode_;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetQSyncMode(QSyncMode qsync_mode) {
  ClientLock lock(disp_mutex_);

  if (!client_ctx_.hw_panel_info.qsync_support) {
    DLOGW("Failed: qsync_support: %d", client_ctx_.hw_panel_info.qsync_support);
    return kErrorNotSupported;
  }

  // force clear qsync mode if set by idle timeout.
  if (qsync_mode_ ==  active_qsync_mode_ && qsync_mode_ == qsync_mode) {
    DLOGW("Qsync mode already set as requested mode: qsync_mode_=%d", qsync_mode_);
    return kErrorNone;
  }

  qsync_mode_ = qsync_mode;
  needs_avr_update_.set(kUpdateAVRModeFlag);
  validated_ = false;
  event_handler_->Refresh();
  return kErrorNone;
}

DisplayError DisplayBuiltIn::ControlIdlePowerCollapse(bool enable, bool synchronous) {
  ClientLock lock(disp_mutex_);
  if (!active_) {
    DLOGW("Invalid display state = %d on display %d-%d. Panel must be on.", state_, display_id_,
          display_type_);
    return kErrorPermission;
  }

  if ((client_ctx_.hw_panel_info.mode == kModeCommand) || client_ctx_.hw_panel_info.vhm_support) {
    validated_ = false;
    return dpu_core_mux_->ControlIdlePowerCollapse(enable, synchronous);
  }

  return kErrorNotSupported;
}

DisplayError DisplayBuiltIn::GetSupportedDSIClock(std::vector<uint64_t> *bitclk_rates) {
  ClientLock lock(disp_mutex_);
  if (!client_ctx_.hw_panel_info.dyn_bitclk_support) {
    return kErrorNotSupported;
  }

  *bitclk_rates = client_ctx_.hw_panel_info.bitclk_rates;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetJitterConfig(uint32_t jitter_type, float value, uint32_t time) {
  ClientLock lock(disp_mutex_);
  if (!active_) {
    DLOGW("Invalid display state = %d. Panel must be on.", state_);
    return kErrorNone;
  }

  if (jitter_type > 2 || (value > 10.0f || value < 0.0f)) {
    return kErrorNotSupported;
  }

  DLOGV("Setting jitter configuration; jitter_type: %d, jitter_val: %lf, jitter_time: %d",
        jitter_type, value, time);
  return hw_intf_->SetJitterConfig(jitter_type, value, time);
}

DisplayError DisplayBuiltIn::SetDynamicDSIClock(uint64_t bit_clk_rate) {
  ClientLock lock(disp_mutex_);
  if (!active_) {
    DLOGW("Invalid display state = %d on display %d-%d. Panel must be on.", state_, display_id_,
          display_type_);
    return kErrorNone;
  }

  if (!client_ctx_.hw_panel_info.dyn_bitclk_support) {
    return kErrorNotSupported;
  }

  uint64_t current_clk = 0;
  std::vector<uint64_t> &clk_rates = client_ctx_.hw_panel_info.bitclk_rates;
  GetDynamicDSIClock(&current_clk);
  bool valid = std::find(clk_rates.begin(), clk_rates.end(), bit_clk_rate) != clk_rates.end();
  if (current_clk == bit_clk_rate || !valid) {
    DLOGI("Invalid setting %d, Clk. already set %d", !valid, (current_clk == bit_clk_rate));
    return kErrorNone;
  }

  validated_ = false;
  avoid_qsync_mode_change_ = true;
  DLOGV("Setting new dynamic bit clk value: %" PRIu64, bit_clk_rate);
  return dpu_core_mux_->SetDynamicDSIClock(bit_clk_rate);
}

DisplayError DisplayBuiltIn::GetDynamicDSIClock(uint64_t *bit_clk_rate) {
  ClientLock lock(disp_mutex_);
  if (!client_ctx_.hw_panel_info.dyn_bitclk_support) {
    return kErrorNotSupported;
  }

  return dpu_core_mux_->GetDynamicDSIClock(bit_clk_rate);
}

DisplayError DisplayBuiltIn::GetRefreshRate(uint32_t *refresh_rate) {
  *refresh_rate = current_refresh_rate_;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetBLScale(uint32_t level) {
  ClientLock lock(disp_mutex_);

  DisplayError err = dpu_core_mux_->SetBLScale(level);
  if (err) {
    DLOGE("Failed to set backlight scale to level %d", level);
  } else {
    DLOGI_IF(kTagDisplay, "Setting backlight scale on display %d-%d to level %d", display_id_,
             display_type_, level);
  }
  return err;
}

bool DisplayBuiltIn::CanCompareFrameROI(LayerStack *layer_stack) {
  // Check Display validation and safe-mode states.
  if (needs_validate_ || comp_manager_->IsSafeMode() || layer_stack->needs_validate) {
    return false;
  }

  // Check Panel and Layer Stack attributes.
  int8_t stack_fudge_factor = 1;  // GPU Target Layer always present in input
  if (layer_stack->flags.stitch_present)
    stack_fudge_factor++;
  if (layer_stack->flags.demura_present)
    stack_fudge_factor++;

  if (!client_ctx_.hw_panel_info.partial_update || (client_ctx_.hw_panel_info.left_roi_count != 1)
      || layer_stack->flags.geometry_changed || layer_stack->flags.skip_present ||
      (layer_stack->layers.size() !=
       (disp_layer_stack_->stack_info.app_layer_count + stack_fudge_factor))) {
    return false;
  }

  // Check for Partial Update disable requests/scenarios.
  if (color_mgr_ && color_mgr_->NeedsPartialUpdateDisable()) {
    DisablePartialUpdateOneFrameInternal();
  }

  if (!partial_update_control_ || disable_pu_one_frame_) {
    return false;
  }

  bool surface_damage = false;
  uint32_t surface_damage_mask_value = (1 << kSurfaceDamage);
  for (uint32_t i = 0; i < layer_stack->layers.size(); i++) {
    Layer *layer = layer_stack->layers.at(i);
    if (layer->update_mask.none()) {
      continue;
    }
    // Only kSurfaceDamage bit should be set in layer's update-mask.
    if (layer->update_mask.to_ulong() == surface_damage_mask_value) {
      surface_damage = true;
    } else {
      return false;
    }
  }

  return surface_damage;
}

bool DisplayBuiltIn::CanSkipDisplayPrepare(LayerStack *layer_stack) {
  if (!CanCompareFrameROI(layer_stack)) {
    return false;
  }

  if (disp_layer_stack_->stack_info.iwe_target_index != -1) {
    return false;
  }

  for (auto& info : disp_layer_stack_->info) {
    info.second.left_frame_roi.clear();
    info.second.right_frame_roi.clear();
    info.second.dest_scale_info_map.clear();
  }
  comp_manager_->GenerateROI(display_comp_ctx_, disp_layer_stack_);

  for (int i = 0; i < core_count_; i++) {
    uint32_t core_id = hw_resource_info_[i].core_id;
    if (!disp_layer_stack_->info.at(core_id).left_frame_roi.size() ||
        !disp_layer_stack_->info.at(core_id).right_frame_roi.size()) {
      return false;
    }

    // Compare the cached and calculated Frame ROIs.
    bool same_roi = IsCongruent(left_frame_roi_[i],
                                disp_layer_stack_->info.at(core_id).left_frame_roi.at(0)) &&
                    IsCongruent(right_frame_roi_[i],
                                disp_layer_stack_->info.at(core_id).right_frame_roi.at(0));

    if (!same_roi) {
      return same_roi;
    }
  }

  for (auto& info : disp_layer_stack_->info) {
    // Update Surface Damage rectangle(s) in HW layers.
    uint32_t hw_layer_count = UINT32(info.second.hw_layers.size());
    for (uint32_t j = 0; j < hw_layer_count; j++) {
      Layer &hw_layer = info.second.hw_layers.at(j);
      Layer *sdm_layer = layer_stack->layers.at(info.second.index.at(j));
      if (hw_layer.dirty_regions.size() != sdm_layer->dirty_regions.size()) {
        return false;
      }
      for (uint32_t k = 0; k < hw_layer.dirty_regions.size(); k++) {
        hw_layer.dirty_regions.at(k) = sdm_layer->dirty_regions.at(k);
      }
    }

    // Set the composition type for SDM layers.
    size_t size_ff = 1;  // GPU Target Layer always present in input
    if (layer_stack->flags.stitch_present)
      size_ff++;
    if (layer_stack->flags.demura_present)
      size_ff++;
    if (disp_layer_stack_->stack_info.common_info.flags.noise_present)
      size_ff++;

    for (uint32_t j = 0; j < (layer_stack->layers.size() - size_ff); j++) {
      layer_stack->layers.at(j)->composition = kCompositionSDE;
    }
  }

  return true;
}

DisplayError DisplayBuiltIn::HandleDemuraLayer(LayerStack *layer_stack) {
  if (!layer_stack) {
    DLOGE("layer_stack is null");
    return kErrorParameters;
  }
  std::vector<Layer *> &layers = layer_stack->layers;
  if (comp_manager_->GetDemuraStatus() && comp_manager_->GetDemuraStatusForDisplay(display_id_) &&
      demura_layer_[0].input_buffer.planes[0].fd > 0) {
    if (disp_layer_stack_->stack_info.demura_target_index == -1) {
      // If demura layer added for first time, do not skip validate
      needs_validate_ = true;
    }

    for (int buf_idx = 0; buf_idx < demura_layer_.size(); buf_idx++) {
      layers.push_back(&demura_layer_.at(buf_idx));
    }

    DLOGI_IF(kTagDisplay, "Demura layer added to layer stack on display %d-%d", display_id_,
             display_type_);
  } else if (disp_layer_stack_->stack_info.demura_target_index != -1) {
    // Demura was present last frame but is now disabled
    needs_validate_ = true;
    disp_layer_stack_->stack_info.demura_present = false;
    DLOGD_IF(kTagDisplay, "Demura layer to be removed on display %d-%d in this frame",
             display_id_, display_type_);
  }
  return kErrorNone;
}

DisplayError DisplayBuiltIn::UpdateTransferTime(uint32_t transfer_time) {
  DisplayError error = kErrorNone;
  {
    ClientLock lock(disp_mutex_);

    if (!active_) {
      DLOGW("Invalid display state = %d. Panel must be on.", state_);
      return kErrorNotSupported;
    }

    if (transfer_time == client_ctx_.hw_panel_info.transfer_time_us) {
      DLOGW("Same transfer time requested. Current = %d, Requested = %d",
            client_ctx_.hw_panel_info.transfer_time_us, transfer_time);
      return kErrorNone;
    } else if (transfer_time > client_ctx_.hw_panel_info.transfer_time_us_max ||
               transfer_time < client_ctx_.hw_panel_info.transfer_time_us_min) {
      DLOGW(
          "Invalid transfer time requested or panel info missing valid range. Min = %d, Max = %d, "
          "Requested = %d, Current = %d",
          client_ctx_.hw_panel_info.transfer_time_us_min,
          client_ctx_.hw_panel_info.transfer_time_us_max, transfer_time,
          client_ctx_.hw_panel_info.transfer_time_us);
      return kErrorParameters;
    }

    error = hw_intf_->UpdateTransferTime(transfer_time);
    if (error != kErrorNone) {
      DLOGW("Retaining the older transfer time.");
      return error;
    }

    DLOGV_IF(kTagDisplay, "Updated transfer time to %d", transfer_time);

    DisplayBase::ReconfigureDisplay();
  }

  event_handler_->Refresh();

  return error;
}

DisplayError DisplayBuiltIn::BuildLayerStackStats(LayerStack *layer_stack) {
  std::vector<Layer *> &layers = layer_stack->layers;
  LayerStackInfo &stack_info = disp_layer_stack_->stack_info;
  stack_info.app_layer_count = 0;
  stack_info.gpu_target_index = -1;
  stack_info.stitch_target_index = -1;
  stack_info.demura_target_index = -1;
  stack_info.noise_layer_index = -1;
  stack_info.cwb_target_index = -1;

  disp_layer_stack_->stack = layer_stack;
  stack_info.common_info.flags = layer_stack->flags;
  stack_info.common_info.blend_cs = layer_stack->blend_cs;
  stack_info.wide_color_primaries.clear();
  stack_info.enable_cac = enable_cac_;
  stack_info.enable_anamorphic_fov = IsAnamorphicFoveationEnabled(layer_stack);
  stack_info.cac_config = cac_config_;
  stack_info.rgba_split_enable = rgba_split_enable_;

  int index = 0;
  for (auto &layer : layers) {
    if (layer->buffer_map == nullptr) {
      layer->buffer_map = std::make_shared<LayerBufferMap>();
    }
    if (layer->composition == kCompositionGPUTarget) {
      stack_info.gpu_target_index = index;
    } else if (layer->composition == kCompositionStitchTarget) {
      stack_info.stitch_target_index = index;
      disp_layer_stack_->stack->flags.stitch_present = true;
      stack_info.stitch_present = true;
    } else if (layer->composition == kCompositionDemura && stack_info.demura_target_index == -1) {
      stack_info.demura_target_index = index;
      disp_layer_stack_->stack->flags.demura_present = true;
      stack_info.demura_present = true;
      DLOGD_IF(kTagDisplay, "Display %d-%d shall request Demura in this frame", display_id_,
               display_type_);
    } else if (layer->composition == kCompositionDemura) {
      stack_info.udc_present = true;
      DLOGV_IF(kTagDisplay, "Adding Aiqe ABC feature - UDC layer");
    } else if (layer->flags.is_noise) {
      stack_info.common_info.flags.noise_present = true;
      stack_info.noise_layer_index = index;
      stack_info.noise_layer_info = noise_layer_info_;
      DLOGV_IF(kTagDisplay, "Display %d-%d requested Noise at index = %d with zpos_n = %d",
               display_id_, display_type_, index, noise_layer_info_.zpos_noise);
    } else if (layer->composition == kCompositionCWBTarget) {
      stack_info.cwb_target_index = index;
      stack_info.cwb_present = true;
    } else {
      stack_info.app_layer_count++;
    }
    if (IsWideColor(layer->input_buffer.dataspace.colorPrimaries)) {
      stack_info.wide_color_primaries.push_back(
          layer->input_buffer.dataspace.colorPrimaries);
    }
    if (layer->flags.is_game) {
      stack_info.game_present = true;
    }
    index++;
  }

  DLOGI_IF(kTagDisplay, "LayerStack layer_count: %zu, app_layer_count: %d "
            "gpu_target_index: %d, stitch_index: %d demura_index: %d cwb_target_index: %d "
            "game_present: %d noise_present: %d display: %d-%d", layers.size(),
            stack_info.app_layer_count, stack_info.gpu_target_index,
            stack_info.stitch_target_index, stack_info.demura_target_index,
            stack_info.cwb_target_index, stack_info.game_present,
            stack_info.common_info.flags.noise_present, display_id_, display_type_);

  if (!stack_info.app_layer_count) {
    DLOGW("Layer count is zero");
    return kErrorNoAppLayers;
  }

  if (stack_info.gpu_target_index > 0) {
    return ValidateGPUTargetParams();
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetActiveConfig(uint32_t index) {
  deferred_config_.MarkDirty();

  if (vrr_enabled_) {
    // Set VRR State
    avr_step_ = hw_intf_->GetAVRStep(index);
    SetQSyncMode(avr_step_ ? kQSyncModeContinuous : kQSyncModeNone);
    SetAVRStepState(avr_step_ != 0);
    SetAvrStepFpsState(index, avr_step_ != 0);
  }

  auto error = DisplayBase::SetActiveConfig(index);
  shared_ptr<Fence> release_fence = nullptr;
  HWDMSType dms_type = client_ctx_.hw_panel_info.dms_type;
  if (dms_type == sdm::HWDMSType::kDMSVIDNonSeamless) {
    SetDisplayState(kStateOff, 0, &release_fence);
    sleep(1);
    SetDisplayState(kStateOn, 0, &release_fence);
  }
  return error;
}

DisplayError DisplayBuiltIn::ReconfigureDisplay() {
  DisplayError error = kErrorNone;
  HWDisplayAttributes display_attributes;
  HWMixerAttributes mixer_attributes;
  HWPanelInfo hw_panel_info;
  uint32_t active_index = 0;

  DTRACE_SCOPED();

  DisplayClientContext client_ctx = {};
  DisplayDeviceContext device_ctx;

  client_ctx = client_ctx_;
  device_ctx = device_ctx_;

  error = dpu_core_mux_->GetActiveConfig(&active_index);
  if (error != kErrorNone) {
    return error;
  }

  error = dpu_core_mux_->GetDisplayAttributes(active_index, &device_ctx, &client_ctx);
  if (error != kErrorNone) {
    return error;
  }

  error = dpu_core_mux_->GetMixerAttributes(&device_ctx, &client_ctx);
  if (error != kErrorNone) {
    return error;
  }

  error = dpu_core_mux_->GetHWPanelInfo(&device_ctx, &client_ctx);
  if (error != kErrorNone) {
    return error;
  }

  display_attributes = client_ctx.display_attributes;
  mixer_attributes = client_ctx.mixer_attributes;
  hw_panel_info = client_ctx.hw_panel_info;

  const bool dirty = deferred_config_.IsDirty();
  if (deferred_config_.IsDeferredState()) {
    if (dirty) {
      SetDeferredFpsConfig();
    } else {
      // In Deferred state, use current config for comparison.
      GetFpsConfig(&display_attributes, &hw_panel_info);
    }
  }

  const bool display_unchanged = (display_attributes == client_ctx_.display_attributes);
  const bool mixer_unchanged = (mixer_attributes == client_ctx_.mixer_attributes);
  const bool panel_unchanged = (hw_panel_info == client_ctx_.hw_panel_info);
  if (!dirty && display_unchanged && mixer_unchanged && panel_unchanged) {
    return kErrorNone;
  }

  if (CanDeferFpsConfig(display_attributes.fps)) {
    deferred_config_.Init(display_attributes.fps, display_attributes.vsync_period_ns,
                          hw_panel_info.transfer_time_us);

    // Apply current config until new Fps is deferred.
    GetFpsConfig(&display_attributes, &hw_panel_info);
  }

  error = comp_manager_->ReconfigureDisplay(display_comp_ctx_, device_ctx, client_ctx,
                                            &cached_qos_data_);
  if (error != kErrorNone) {
    return error;
  }
  for (auto& qos_data : cached_qos_data_) {
    default_clock_hz_.at(qos_data.first) = qos_data.second.clock_hz;
  }

  // Disable Partial Update for one frame as PU not supported during modeset.
  DisablePartialUpdateOneFrameInternal();

  client_ctx_.display_attributes = display_attributes;
  client_ctx_.mixer_attributes = mixer_attributes;
  client_ctx_.hw_panel_info = hw_panel_info;
  device_ctx_ = device_ctx;

  if (client_ctx_.hw_panel_info.partial_update) {
    // If current panel supports Partial Update, then add a pending PU request
    // to be served in the first PU enable frame after the modeset frame.
    // Because if first PU enable frame, after transition, has a partial Frame-ROI and
    // is followed by Skip Validate frames, then it can benefit those frames.
    pu_pending_ = true;
  }

  if (enable_dpps_dyn_fps_) {
    uint32_t dpps_fps = client_ctx_.display_attributes.fps;
    DppsNotifyPayload dpps_payload = {};
    dpps_payload.is_primary = IsPrimaryDisplayLocked();
    dpps_payload.payload = &dpps_fps;
    dpps_payload.payload_size = sizeof(dpps_fps);
    dpps_info_.DppsNotifyOps(kDppsUpdateFpsEvent, &dpps_payload, sizeof(dpps_payload));
  }

  // Notify Demura when refresh rate changes
  if (demura_ && !abc_prop_) {
    GenericPayload demura_fps_pl = {};
    uint32_t *demura_fps_ptr = nullptr;
    int ret = demura_fps_pl.CreatePayload<uint32_t>(demura_fps_ptr);
    if (ret) {
      DLOGE("Failed to create payload for demura fps, error = %d", ret);
    } else {
      *demura_fps_ptr = client_ctx_.display_attributes.fps;
      ret = demura_->SetParameter(kDemuraFeatureParamRefreshRate, demura_fps_pl);
      if (ret) {
        DLOGE("Failed to set refresh rate for demura, error = %d", ret);
      }
    }
  }
  return kErrorNone;
}

bool DisplayBuiltIn::CanDeferFpsConfig(uint32_t fps) {
  if (deferred_config_.CanApplyDeferredState()) {
    // Deferred Fps Config needs to be applied.
    return false;
  }

  // In case of higher to lower Fps transition on a Builtin display, defer the Fps
  // (Transfer time) configuration, for the number of frames based on frame_count.
  return ((deferred_config_.frame_count != 0) && (client_ctx_.display_attributes.fps > fps));
}

void DisplayBuiltIn::SetDeferredFpsConfig() {
  // Update with the deferred Fps Config.
  client_ctx_.display_attributes.fps = deferred_config_.fps;
  client_ctx_.display_attributes.vsync_period_ns = deferred_config_.vsync_period_ns;
  client_ctx_.hw_panel_info.transfer_time_us = deferred_config_.transfer_time_us;
  for (uint32_t i = 0; i < device_ctx_.size(); i++) {
    device_ctx_[i].display_attributes.fps = deferred_config_.fps;
    device_ctx_[i].display_attributes.vsync_period_ns = deferred_config_.vsync_period_ns;
    device_ctx_[i].hw_panel_info.transfer_time_us = deferred_config_.transfer_time_us;
  }
  deferred_config_.Clear();
}

void DisplayBuiltIn::GetFpsConfig(HWDisplayAttributes *display_attr, HWPanelInfo *panel_info) {
  display_attr->fps = client_ctx_.display_attributes.fps;
  display_attr->vsync_period_ns = client_ctx_.display_attributes.vsync_period_ns;
  panel_info->transfer_time_us = client_ctx_.hw_panel_info.transfer_time_us;
}

PrimariesTransfer DisplayBuiltIn::GetBlendSpaceFromStcColorMode(
    const snapdragoncolor::ColorMode &color_mode) {
  PrimariesTransfer blend_space = {};
  if (!color_mgr_) {
    return blend_space;
  }

  // Set sRGB as default blend space.
  bool native_mode = (color_mode.intent == snapdragoncolor::kNative) ||
                     (color_mode.gamut == ColorPrimaries_Max && color_mode.gamma == Transfer_Max);
  if (stc_color_modes_.list.empty() || (native_mode && allow_tonemap_native_)) {
    return blend_space;
  }

  blend_space.primaries = qti_primaries_map[color_mode.gamut];
  blend_space.transfer = qti_transfer_map[color_mode.gamma];

  return blend_space;
}

DisplayError DisplayBuiltIn::GetConfig(DisplayConfigFixedInfo *fixed_info) {
  ClientLock lock(disp_mutex_);
  fixed_info->is_cmdmode = (client_ctx_.hw_panel_info.mode == kModeCommand);
  fixed_info->vhm_support = client_ctx_.hw_panel_info.vhm_support;
  bool hdr_supported = true;
  bool has_concurrent_writeback = true;

  for (auto info_intf = hw_info_intf_.Begin(); info_intf != hw_info_intf_.End(); info_intf++) {
    HWResourceInfo hw_resource_info = HWResourceInfo();
    info_intf->second->GetHWResourceInfo(&hw_resource_info);
    hdr_supported &= hw_resource_info.has_hdr;
    uint32_t core_id = hw_resource_info.core_id;
    has_concurrent_writeback &= hw_resource_info.has_concurrent_writeback;
    has_concurrent_writeback &= device_ctx_[core_id].hw_panel_info.hdr_enabled;
  }

  bool hdr_plus_supported = false;
  bool dolby_vision_supported = false;

  // Checking library support for HDR10+
  comp_manager_->GetHDRCapability(&hdr_plus_supported, &dolby_vision_supported);

  fixed_info->hdr_supported = hdr_supported;
  // Built-in displays always support HDR10+ when the target supports HDR
  fixed_info->hdr_plus_supported = fixed_info->hdr_supported && hdr_plus_supported;
  fixed_info->dolby_vision_supported = fixed_info->hdr_supported && dolby_vision_supported;
  // Populate luminance values only if hdr will be supported on that display
  fixed_info->max_luminance = fixed_info->hdr_supported ?
                              client_ctx_.hw_panel_info.peak_luminance: 0;
  fixed_info->average_luminance = fixed_info->hdr_supported ?
                                  client_ctx_.hw_panel_info.average_luminance : 0;
  fixed_info->min_luminance = fixed_info->hdr_supported ?
                              client_ctx_.hw_panel_info.blackness_level: 0;
  fixed_info->hdr_eotf = client_ctx_.hw_panel_info.hdr_eotf;
  fixed_info->hdr_metadata_type_one = client_ctx_.hw_panel_info.hdr_metadata_type_one;
  fixed_info->partial_update = client_ctx_.hw_panel_info.partial_update;
  fixed_info->readback_supported =
      has_concurrent_writeback && !(kQuadSplit == client_ctx_.mixer_attributes.split_type);
  fixed_info->supports_unified_draw = unified_draw_supported_;

  return kErrorNone;
}

void DisplayBuiltIn::SendBacklight() {
  DisplayError err = kErrorNone;
  int level = 0;
  if ((err = dpu_core_mux_->GetPanelBrightness(&level)) != kErrorNone) {
    return;
  }
  HandleBacklightEvent(level);
}

void DisplayBuiltIn::SendDisplayConfigs() {
  if (ipc_intf_) {
    GenericPayload in;
    uint32_t active_index = 0;
    IPCDisplayConfigParams *disp_configs = nullptr;
    int ret = in.CreatePayload<IPCDisplayConfigParams>(disp_configs);
    if (ret) {
      DLOGW("failed to create the payload. Error:%d", ret);
      return;
    }
    DisplayError error = dpu_core_mux_->GetActiveConfig(&active_index);
    if (error != kErrorNone) {
      return;
    }
    disp_configs->h_total = client_ctx_.display_attributes.h_total;
    disp_configs->v_total = client_ctx_.display_attributes.v_total;
    disp_configs->fps = client_ctx_.display_attributes.fps;
    disp_configs->smart_panel = client_ctx_.display_attributes.smart_panel;
    disp_configs->is_primary = IsPrimaryDisplayLocked();
    disp_configs->mixer_width = client_ctx_.mixer_attributes.width;
    disp_configs->mixer_height = client_ctx_.mixer_attributes.height;

    if (abc_prop_ && demura_) {
      GenericPayload in_payload;
      DemuraFeatureParamConfigIdx<std::string> *config_mode_name = nullptr;
      int rc = in_payload.CreatePayload(config_mode_name);
      if (rc != 0) {
        DLOGE("Failed to create payload for config_mode_name, error = %d", rc);
        return;
      }

      int error = demura_->GetParameter(kDemuraFeatureParamGetMode, &in_payload);
      if (error) {
        DLOGE("Failed to get reconfig, error %d", ret);
        return;
      }

      disp_configs->abc_mode = config_mode_name->modeinfo;
      DLOGI("current_abc_mode = %s", disp_configs->abc_mode.c_str());
    }

    if (enable_ai_scaler_) {
      uint32_t current_mode_id = 0;
      DisplayError error = comp_manager_->GetAIScalerMode(&current_mode_id);
      if (error) {
        DLOGE("Failed to get current AI scaler mode ID, error = %d", error);
        return;
      }

      disp_configs->ai_scaler_mode_id = current_mode_id;
      DLOGI("Current AI Scaler mode ID = %d", disp_configs->ai_scaler_mode_id);
    }

    if ((ret = ipc_intf_->SetParameter(kIpcParamDisplayConfigs, in))) {
      DLOGW("Failed to send display config, error = %d", ret);
    }
  }
}

int DisplayBuiltIn::SetDemuraIntfStatus(bool enable, int current_idx) {
  int ret = 0;
  bool *reconfig = nullptr;
  GenericPayload reconfig_pl;
  if (!demura_) {
    DLOGE("demura_ is nullptr");
    return -EINVAL;
  }

  if (enable) {
    if ((ret = reconfig_pl.CreatePayload<bool>(reconfig))) {
      DLOGE("Failed to create payload for reconfig, error = %d", ret);
      return ret;
    }

    ret = demura_->GetParameter(kDemuraFeatureParamPendingReconfig, &reconfig_pl);
    if (ret) {
      DLOGE("Failed to get reconfig, error %d", ret);
      return ret;
    }

    if (*reconfig) {
      DLOGI("SetDemuraLayer for Anti-Aging reconfig");
      // TBD: handle for ABC during reconfig verification
      ret = SetupCorrectionLayer();
      if (ret) {
        DLOGE("Failed to setup Demura layer, error %d", ret);
        return ret;
      }
    }
  }

  GenericPayload config_pl;
  if (abc_prop_) {
    DemuraFeatureParamConfigIdx<std::string> *config_mode_name = nullptr;
    if ((ret = config_pl.CreatePayload(config_mode_name))) {
      DLOGE("Failed to create payload for config_mode_name, error = %d", ret);
      return ret;
    }

    config_mode_name->modeinfo = "";
    if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, config_pl))) {
      DLOGE("Failed to set Config Idx, error = %d", ret);
      return ret;
    }

    if (SetupCorrectionLayer() != kErrorNone) {
      DLOGE("Unable to setup abc_intf layer on Display %d-%d", display_id_, display_type_);
      return kErrorUndefined;
    }

  } else {
    uConfigIdx *config_idx = nullptr;
    if ((ret = config_pl.CreatePayload<uConfigIdx>(config_idx))) {
      DLOGE("Failed to create payload for config_idx, error = %d", ret);
      return ret;
    }

    config_idx->modeinfo = current_idx;
    if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, config_pl))) {
      DLOGE("Failed to set Config Idx, error = %d", ret);
      return ret;
    }
  }

  GenericPayload pl;
  bool* enable_ptr = nullptr;
  if ((ret = pl.CreatePayload<bool>(enable_ptr))) {
    DLOGE("Failed to create payload for enable, error = %d", ret);
    return ret;
  } else {
    *enable_ptr = enable;
    if ((ret = demura_->SetParameter(kDemuraFeatureParamActive, pl))) {
      DLOGE("Failed to set Active, error = %d", ret);
      return ret;
    }
  }

  if (enable && reconfig && (*reconfig)) {
    *reconfig = false;
    ret = demura_->SetParameter(kDemuraFeatureParamPendingReconfig, reconfig_pl);
    if (ret) {
      DLOGE("Failed to set reconfig, error %d", ret);
      return ret;
    }
  }
  demura_current_idx_ = current_idx;
  DLOGI("Demura is now %s and current index is %d ", enable ? "Enabled" : "Disabled", current_idx);
  return ret;
}

DisplayError DisplayBuiltIn::SetDppsFeatureLocked(void *payload, size_t size) {
  return dpu_core_mux_->SetDppsFeature(payload, size);
}

void DisplayBuiltIn::HandlePowerEvent() {
  return ProcessPowerEvent();
}

void DisplayBuiltIn::HandleVmReleaseEvent() {
  if (event_handler_)
    event_handler_->HandleEvent(kVmReleaseDone);
}

void DisplayBuiltIn::HandleVmReclaimEvent() {
  if (event_handler_)
    event_handler_->HandleEvent(kVmReclaimDone);
}

DisplayError DisplayBuiltIn::GetQsyncFps(uint32_t *qsync_fps) {
  ClientLock lock(disp_mutex_);
  return dpu_core_mux_->GetQsyncFps(qsync_fps);
}

DisplayError DisplayBuiltIn::SetAlternateDisplayConfig(uint32_t *alt_config) {
  ClientLock lock(disp_mutex_);
  if (!alt_config) {
    return kErrorResources;
  }
  DisplayError error = dpu_core_mux_->SetAlternateDisplayConfig(alt_config);

  if (error == kErrorNone) {
    ReconfigureDisplay();
    validated_ = false;
  }

  return error;
}

// LCOV_EXCL_START
DisplayError DisplayBuiltIn::HandleSecureEvent(SecureEvent secure_event, bool *needs_refresh) {
  DisplayError error = kErrorNone;

  error = DisplayBase::HandleSecureEvent(secure_event, needs_refresh);
  if (error) {
    if (error == kErrorPermission) {
      DLOGW("Failed to handle secure event %d", secure_event);
    } else {
      DLOGE("Failed to handle secure event %d", secure_event);
    }
    return error;
  }

  if (secure_event == kTUITransitionEnd &&
      ((demura_intended_ && demura_dynamic_enabled_) || abc_enabled_)) {
    // enable demura after TUI transition end
    SetDemuraIntfStatus(true, demura_current_idx_);
    comp_manager_->SetDemuraStatusForDisplay(display_id_, true);
  }

  return error;
}

DisplayError DisplayBuiltIn::PostHandleSecureEvent(SecureEvent secure_event) {
  ClientLock lock(disp_mutex_);
  if (secure_event == kTUITransitionStart) {
    if (!pending_brightness_) {
      if (secure_event == kTUITransitionStart) {
        // Send the panel brightness event to secondary VM on TUI session start
        SendBacklight();
      }
    }
    if (secure_event == kTUITransitionStart) {
      // Send display config information to secondary VM on TUI session start
      SendDisplayConfigs();
    }

    if (secure_event == kTUITransitionStart &&
        comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
      comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
      //  disable demura before TUI transition start
      SetDemuraIntfStatus(false, demura_current_idx_);
    }
  }
  if (secure_event == kTUITransitionEnd) {
    comp_manager_->PostHandleSecureEvent(display_comp_ctx_, secure_event);
    event_handler_->Refresh();
  }
  return kErrorNone;
}

DisplayIPCVmCallbackImpl::DisplayIPCVmCallbackImpl(BufferAllocator *buffer_allocator,
                                                       std::shared_ptr<IPCIntf> ipc_intf,
                                                       uint64_t panel_id, uint32_t width,
                                                       uint32_t height)
  : buffer_allocator_(buffer_allocator),  ipc_intf_(ipc_intf), panel_id_(panel_id),
    hfc_buffer_width_(width), hfc_buffer_height_(height) {}

void DisplayIPCVmCallbackImpl::Init() {
  if (!ipc_intf_) {
    DLOGW("IPC interface is NULL");
    return;
  }
  GenericPayload in_reg;
  DisplayIPCVmCallbackImpl **cb_intf = nullptr;
  int ret = in_reg.CreatePayload<DisplayIPCVmCallbackImpl *>(cb_intf);
  if (ret) {
    DLOGE("failed to create the payload for in_reg. Error:%d", ret);
    return;
  }
  *cb_intf = this;
  GenericPayload out_reg;
  ret = out_reg.CreatePayload<int>(cb_hnd_out_);
  if (ret) {
    DLOGE("failed to create the payload for out_reg. Error:%d", ret);
    return;
  }
  if ((ret = ipc_intf_->ProcessOps(kIpcOpsRegisterVmCallback, in_reg, &out_reg))) {
    DLOGE("Failed to register vm callback, error = %d", ret);
    return;
  }
}
void DisplayIPCVmCallbackImpl::Deinit() {
  if (!ipc_intf_) {
    DLOGW("IPC interface is NULL");
    return;
  }
  GenericPayload in_unreg;
  int *cb_hnd_in = nullptr;
  int ret = in_unreg.CreatePayload<int>(cb_hnd_in);
  if (ret) {
    DLOGE("failed to create the payload for in_unreg. Error:%d", ret);
    return;
  }
  *cb_hnd_in = *cb_hnd_out_;
  if ((ret = ipc_intf_->ProcessOps(kIpcOpsUnRegisterVmCallback, in_unreg, nullptr))) {
    DLOGE("Failed to unregister vm callback, error = %d", ret);
    return;
  }
}
void DisplayIPCVmCallbackImpl::OnServerReady() {
  lock_guard<recursive_mutex> obj(cb_mutex_);
  server_ready_ = true;
}

void DisplayIPCVmCallbackImpl::ExportHFCBuffer() {
  lock_guard<recursive_mutex> obj(cb_mutex_);
  if (!server_ready_) {
    DLOGW("Server not ready, Failed to export HFC buffers");
    return;
  }

  if (!ipc_intf_ || !buffer_allocator_) {
    DLOGE("Invalid parameters ipc_intf_ %p, buffer_allocator_ %p", ipc_intf_.get(),
          buffer_allocator_);
    return;
  }

  buffer_info_hfc_.buffer_config.width = hfc_buffer_width_;
  buffer_info_hfc_.buffer_config.height = hfc_buffer_height_;
  buffer_info_hfc_.buffer_config.format = kFormatBGRA8888;
  buffer_info_hfc_.buffer_config.buffer_count = 1;
  std::bitset<kBufferPermMax> buf_perm;
  buf_perm.set(kBufferPermRead);
  buf_perm.set(kBufferPermWrite);
  buffer_info_hfc_.buffer_config.access_control.insert(
      std::make_pair(kBufferClientUnTrustedVM, buf_perm));
  buffer_info_hfc_.buffer_config.access_control.insert(
      std::make_pair(kBufferClientTrustedVM, buf_perm));

  int ret = buffer_allocator_->AllocateBuffer(&buffer_info_hfc_);
  if (ret != 0) {
    DLOGE("Fail to allocate hfc buffer");
    return;
  }

  GenericPayload in;
  IPCBufferInfo *export_buf_in_params = nullptr;
  ret = in.CreatePayload<IPCBufferInfo>(export_buf_in_params);
  if (ret) {
    DLOGE("failed to create IPCExportBufInParams payload. Error:%d", ret);
    buffer_allocator_->FreeBuffer(&buffer_info_hfc_);
    return;
  }

  export_buf_in_params->size = buffer_info_hfc_.alloc_buffer_info.size;
  export_buf_in_params->panel_id = panel_id_;
  export_buf_in_params->mem_handle = buffer_info_hfc_.alloc_buffer_info.mem_handle;

  DLOGI("Allocated hfc buffer mem_handle %" PRId64 " size %d panel id :%" PRIX64, export_buf_in_params->mem_handle,
        export_buf_in_params->size, export_buf_in_params->panel_id);
  if ((ret = ipc_intf_->SetParameter(kIpcParamSetHFCBuffer, in))) {
    DLOGE("Failed to export demura buffers, error = %d", ret);
    buffer_allocator_->FreeBuffer(&buffer_info_hfc_);
    return;
  }
}

void DisplayIPCVmCallbackImpl::FreeExportBuffer() {
  lock_guard<recursive_mutex> obj(cb_mutex_);
  buffer_allocator_->FreeBuffer(&buffer_info_hfc_);
  DLOGI("Free hfc export buffer and fd");
}

void DisplayIPCVmCallbackImpl::OnServerExit() {
  lock_guard<recursive_mutex> obj(cb_mutex_);
  server_ready_ = false;
}
// LCOV_EXCL_STOP

CacVersion DisplayBuiltIn::GetCacVerion() {
  int cac_version = 0;
  for (int i = 0; i < core_count_; i++) {
    cac_version |= hw_resource_info_[i].cac_version;
  }

  return static_cast<CacVersion>(cac_version);
}

DisplayError DisplayBuiltIn::AllocateDummyLoopbackCACBuffer() {
  if (GetCacVerion() != kCacVersionLoopback) {
    return kErrorNone;
  }

  dummy_loopback_cac_info_.buffer_config.width = client_ctx_.display_attributes.x_pixels;
  dummy_loopback_cac_info_.buffer_config.height = client_ctx_.display_attributes.y_pixels;

  dummy_loopback_cac_info_.buffer_config.format = kFormatRGBA8888Ubwc;
  dummy_loopback_cac_info_.buffer_config.buffer_count = 1;
  if (buffer_allocator_->AllocateBuffer(&dummy_loopback_cac_info_) != 0) {
    DLOGE("Loopback CAC Buffer allocation failed");
    return kErrorMemory;
  }

  buffer_allocator_->FreeBuffer(&dummy_loopback_cac_info_);
  dummy_loopback_cac_info_.alloc_buffer_info.fd = -1;

  return kErrorNone;
}

void DisplayBuiltIn::InitCWBBuffer() {
  if (client_ctx_.hw_panel_info.mode != kModeVideo || !HasConcurrentWriteback()
      || !client_ctx_.hw_panel_info.is_primary_panel) {
    return;
  }

  is_wb_ubwc_supported_ = true;
  for (auto hw_info = hw_info_intf_.Begin(); hw_info != hw_info_intf_.End(); hw_info++) {
    HWDisplaysInfo display_infos;
    DisplayError error = hw_info->second->GetDisplaysStatus(&display_infos);
    if (error)
      continue;

    bool is_cur_core_wb_ubwc_supported = false;
    for (auto &iter : display_infos) {
      auto &info = iter.second;
      if (info.display_type == kVirtual && info.is_wb_ubwc_supported) {
        is_cur_core_wb_ubwc_supported = true;
        break;
      }
    }
    is_wb_ubwc_supported_ &= is_cur_core_wb_ubwc_supported;
  }

  DLOGV_IF(kTagDisplay, "WB blocks support UBWC output: %d", is_wb_ubwc_supported_);

  if (!disable_fbt_for_cwb_fallback_) {
    return;
  }

  if (disable_cwb_idle_fallback_ || cwb_buffer_initialized_) {
    return;
  }

  // Initialize CWB buffer with display resolution to get full size buffer
  // as mixer or fb can init with custom values based on property
  output_buffer_info_.buffer_config.width = client_ctx_.display_attributes.x_pixels;
  output_buffer_info_.buffer_config.height = client_ctx_.display_attributes.y_pixels;

  if (is_wb_ubwc_supported_) {
    output_buffer_info_.buffer_config.format = kFormatRGBX8888Ubwc;
  } else {
    output_buffer_info_.buffer_config.format = kFormatRGB888;
  }

  output_buffer_info_.buffer_config.buffer_count = 1;
  if (buffer_allocator_->AllocateBuffer(&output_buffer_info_) != 0) {
    DLOGE("Buffer allocation failed");
    return;
  }

  LayerBuffer buffer = {};
  buffer.planes[0].fd = output_buffer_info_.alloc_buffer_info.fd;
  buffer.planes[0].offset = 0;
  buffer.planes[0].stride = output_buffer_info_.alloc_buffer_info.stride;
  buffer.size = output_buffer_info_.alloc_buffer_info.size;
  buffer.handle_id = output_buffer_info_.alloc_buffer_info.id;
  buffer.width = output_buffer_info_.alloc_buffer_info.aligned_width;
  buffer.height = output_buffer_info_.alloc_buffer_info.aligned_height;
  buffer.format = output_buffer_info_.alloc_buffer_info.format;
  buffer.unaligned_width = output_buffer_info_.buffer_config.width;
  buffer.unaligned_height = output_buffer_info_.buffer_config.height;

  cwb_layer_.composition = kCompositionCWBTarget;
  cwb_layer_.input_buffer = buffer;
  cwb_layer_.input_buffer.buffer_id = reinterpret_cast<uint64_t>(output_buffer_info_.private_data);
  cwb_layer_.src_rect = {0, 0, FLOAT(cwb_layer_.input_buffer.unaligned_width),
                         FLOAT(cwb_layer_.input_buffer.unaligned_height)};
  cwb_layer_.dst_rect = {0, 0, FLOAT(cwb_layer_.input_buffer.unaligned_width),
                         FLOAT(cwb_layer_.input_buffer.unaligned_height)};

  cwb_layer_.flags.is_cwb = 1;
  cwb_buffer_initialized_ = true;
  return;
}

void DisplayBuiltIn::DeinitCWBBuffer() {
  if (!disable_fbt_for_cwb_fallback_) {
    CloseFd(&cwb_layer_.input_buffer.planes[0].fd);
    cwb_layer_ = {};
    return;
  }

  if (!cwb_buffer_initialized_) {
    return;
  }

  buffer_allocator_->FreeBuffer(&output_buffer_info_);
  cwb_layer_ = {};
  cwb_buffer_initialized_ = false;
}

void DisplayBuiltIn::UpdateFrameBufferForCWB() {
  if (disable_cwb_idle_fallback_ || disable_fbt_for_cwb_fallback_) {
    return;
  }

  if (!IsFrameBufferPresent()) {
    return;
  }

  for (auto &info : disp_layer_stack_->info) {
    uint32_t hw_layers_count = info.second.hw_layers.size();
    for (uint32_t i = 0; i < hw_layers_count; i++) {
      uint32_t sdm_layer_index = info.second.index.at(i);
      Layer &hw_layer = info.second.hw_layers.at(i);
      if (disp_layer_stack_->stack_info.gpu_target_index == sdm_layer_index) {
        if (curr_framebuffer_.handle_id != hw_layer.input_buffer.handle_id) {
          // fbt flipped
          CloseFd(&prev_framebuffer_.planes[0].fd);
          prev_framebuffer_ = curr_framebuffer_;
          curr_framebuffer_ = hw_layer.input_buffer;
          curr_framebuffer_.planes[0].fd = Sys::dup_(hw_layer.input_buffer.planes[0].fd);
          DLOGV_IF(
              kTagDisplay, "display %d-%d, updating FBT handle_id prev %" PRIu64 " curr %" PRIu64,
              display_id_, display_type_, prev_framebuffer_.handle_id, curr_framebuffer_.handle_id);
        }

        if (disp_layer_stack_->stack_info.update_fbt_for_cwb) {
          UpdateCWBLayer(prev_framebuffer_);
          *(info.second.output_buffer) = cwb_layer_.input_buffer;
          disp_layer_stack_->stack_info.update_fbt_for_cwb = false;
          DLOGV_IF(kTagDisplay,
                   "display %d-%d, updating FBT handle_id %" PRIu64
                   " width %d height %d"
                   " format %d for CWB capture",
                   display_id_, display_type_, cwb_layer_.input_buffer.handle_id,
                   cwb_layer_.input_buffer.unaligned_width,
                   cwb_layer_.input_buffer.unaligned_height, cwb_layer_.input_buffer.format);
        }
      }
    }
  }
}

void DisplayBuiltIn::UpdateCWBLayer(LayerBuffer &layer_buffer) {
  CloseFd(&cwb_layer_.input_buffer.planes[0].fd);
  cwb_layer_.input_buffer = layer_buffer;
  cwb_layer_.input_buffer.planes[0].fd = Sys::dup_(layer_buffer.planes[0].fd);

  if (!is_wb_ubwc_supported_) {
    // use fbt with linear format
    cwb_layer_.input_buffer.format = kFormatRGBA8888;
    cwb_layer_.input_buffer.usage = 0;
  }

  cwb_layer_.src_rect = {0, 0, FLOAT(client_ctx_.fb_config.x_pixels),
                         FLOAT(client_ctx_.fb_config.y_pixels)};
  cwb_layer_.dst_rect = cwb_layer_.src_rect;
  cwb_layer_.composition = kCompositionCWBTarget;
  cwb_layer_.flags.is_cwb = 1;
}

void DisplayBuiltIn::AppendCWBLayerWithFBT(LayerStack *layer_stack) {
  /* if N is the FBT thats currently with the driver (or the latest slot that has been used),
     then use N-1 FBT for capturing CWB dump for idle fallback. N+1 FBT can be used safely
     if idle exit commit needs an fbt flip. */

  // Previous FBT is not yet available for capturing CWB
  if (prev_framebuffer_.handle_id <= 0) {
    return;
  }

  // Check if prev FBT has current FB resolution
  if ((prev_framebuffer_.unaligned_width != client_ctx_.fb_config.x_pixels) ||
      (prev_framebuffer_.unaligned_height != client_ctx_.fb_config.y_pixels)) {
    return;
  }

  // Check if CWB layer is already using prev FBT handle
  if ((cwb_layer_.input_buffer.handle_id > 0) &&
      (cwb_layer_.input_buffer.handle_id == prev_framebuffer_.handle_id)) {
    DLOGV_IF(kTagDisplay,
             "display %d-%d, using FBT handle_id %" PRIu64
             " width %d height %d"
             " format %d for CWB Layer",
             display_id_, display_type_, cwb_layer_.input_buffer.handle_id,
             cwb_layer_.input_buffer.unaligned_width, cwb_layer_.input_buffer.unaligned_height,
             cwb_layer_.input_buffer.format);
    layer_stack->layers.push_back(&cwb_layer_);
    return;
  }

  UpdateCWBLayer(prev_framebuffer_);
  layer_stack->layers.push_back(&cwb_layer_);
  DLOGV_IF(kTagDisplay,
           "display %d-%d, using FBT handle_id %" PRIu64
           " width %d height %d"
           " format %d for CWB Layer",
           display_id_, display_type_, cwb_layer_.input_buffer.handle_id,
           cwb_layer_.input_buffer.unaligned_width, cwb_layer_.input_buffer.unaligned_height,
           cwb_layer_.input_buffer.format);
}

void DisplayBuiltIn::AppendCWBLayer(LayerStack *layer_stack) {
  if (!client_ctx_.hw_panel_info.is_primary_panel || disable_cwb_idle_fallback_) {
    return;
  }

  uint32_t new_mixer_width = client_ctx_.fb_config.x_pixels;
  uint32_t new_mixer_height = client_ctx_.fb_config.y_pixels;
  NeedsMixerReconfiguration(layer_stack, &new_mixer_width, &new_mixer_height);

  if (!disable_fbt_for_cwb_fallback_) {
    // FB resolution needs to be same as mixer resolution to re-use for CWB dump
    if ((new_mixer_width != client_ctx_.fb_config.x_pixels) ||
        (new_mixer_height != client_ctx_.fb_config.y_pixels)) {
      return;
    }

    AppendCWBLayerWithFBT(layer_stack);
    return;
  }

  if (cwb_buffer_initialized_ &&
      (cwb_layer_.input_buffer.unaligned_width < client_ctx_.display_attributes.x_pixels ||
       cwb_layer_.input_buffer.unaligned_height < client_ctx_.display_attributes.y_pixels)) {
    DLOGI("Resetting CWB layer due to insufficient buffer size(%dx%d) compare to output(%dx%d).",
          cwb_layer_.input_buffer.unaligned_width, cwb_layer_.input_buffer.unaligned_height,
          client_ctx_.display_attributes.x_pixels, client_ctx_.display_attributes.y_pixels);
    DeinitCWBBuffer();
  }

  if (!cwb_buffer_initialized_) {
    // If CWB buffer is not initialized, then it must be initialized for video mode
    InitCWBBuffer();
  }

  if (!cwb_buffer_initialized_) {
    return;
  }

  // Set cwb src_rect same as mixer resolution since LM tappoint
  // and dest_rect equal to fb resolution as strategy scales HWLayer dest rect based on fb
  cwb_layer_.src_rect = {0, 0, FLOAT(new_mixer_width), FLOAT(new_mixer_height)};
  cwb_layer_.dst_rect = {0, 0, FLOAT(client_ctx_.fb_config.x_pixels),
                         FLOAT(client_ctx_.fb_config.y_pixels)};
  cwb_layer_.composition = kCompositionCWBTarget;
  layer_stack->layers.push_back(&cwb_layer_);
}

uint32_t DisplayBuiltIn::GetUpdatingAppLayersCount(LayerStack *layer_stack) {
  uint32_t updating_count = 0;

  for (uint i = 0; i < layer_stack->layers.size(); i++) {
    auto layer = layer_stack->layers.at(i);
    if (layer->composition == kCompositionGPUTarget) {
      break;
    }
    if (layer->flags.updating) {
      updating_count++;
    }
  }

  return updating_count;
}

DisplayError DisplayBuiltIn::ChangeFps() {
  ClientLock lock(disp_mutex_);

  if (!active_ || !client_ctx_.hw_panel_info.dynamic_fps || qsync_mode_ != kQSyncModeNone ||
      disable_dyn_fps_) {
    return kErrorNotSupported;
  }

  uint32_t num_updating_layers = GetUpdatingLayersCount();
  bool one_updating_layer = (num_updating_layers == 1);
  uint32_t refresh_rate = GetOptimalRefreshRate(one_updating_layer);

  if (refresh_rate < client_ctx_.hw_panel_info.min_fps ||
      refresh_rate > client_ctx_.hw_panel_info.max_fps) {
    DLOGE("Invalid Fps = %d request", refresh_rate);
    return kErrorParameters;
  }

  bool idle_screen = GetUpdatingAppLayersCount(disp_layer_stack_->stack) == 0;
  if (!disp_layer_stack_->stack->force_refresh_rate && IdleFallbackLowerFps(idle_screen) &&
      !enable_qsync_idle_) {
    refresh_rate = client_ctx_.hw_panel_info.min_fps;
  }

  if (current_refresh_rate_ != refresh_rate) {
    DisplayError error = dpu_core_mux_->SetRefreshRate(refresh_rate);
    if (error != kErrorNone) {
      // Attempt to update refresh rate can fail if rf interference settings is detected.
      // Just drop min fps settting for now.
      if (disp_layer_stack_->stack_info.lower_fps) {
        disp_layer_stack_->stack_info.lower_fps = false;
      }
      return error;
    }

    error = comp_manager_->CheckEnforceSplit(display_comp_ctx_, refresh_rate);
    if (error != kErrorNone) {
      return error;
    }
  }

  // Set safe mode upon success.
  if (enhance_idle_time_ && (refresh_rate == client_ctx_.hw_panel_info.min_fps) &&
      (disp_layer_stack_->stack_info.lower_fps)) {
    comp_manager_->ProcessIdleTimeout(display_comp_ctx_);
  }

  // On success, set current refresh rate to new refresh rate
  current_refresh_rate_ = refresh_rate;
  deferred_config_.MarkDirty();

  return ReconfigureDisplay();
}

bool DisplayBuiltIn::IdleFallbackLowerFps(bool idle_screen) {
  if (!enhance_idle_time_) {
    return (disp_layer_stack_->stack_info.lower_fps);
  }
  if (!idle_screen || !disp_layer_stack_->stack_info.lower_fps) {
    return false;
  }

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  uint64_t elapsed_time_ms = GetTimeInMs(now) - GetTimeInMs(idle_timer_start_);
  bool can_lower = elapsed_time_ms >= UINT32(idle_time_ms_);
  DLOGV_IF(kTagDisplay, "display %d-%d , lower fps: %d", display_id_, display_type_, can_lower);

  return can_lower;
}

uint32_t DisplayBuiltIn::GetUpdatingLayersCount() {
  uint32_t updating_count = 0;

  for (uint i = 0; i < disp_layer_stack_->stack->layers.size(); i++) {
    auto layer = disp_layer_stack_->stack->layers.at(i);
    if (layer->flags.updating) {
      updating_count++;
    }
  }
  return updating_count;
}

uint32_t DisplayBuiltIn::GetOptimalRefreshRate(bool one_updating_layer) {
  LayerStack *layer_stack = disp_layer_stack_->stack;
  if (layer_stack->force_refresh_rate) {
    return layer_stack->force_refresh_rate;
  }

  uint32_t metadata_refresh_rate = CalculateMetaDataRefreshRate();
  if (layer_stack->flags.use_metadata_refresh_rate && one_updating_layer &&
      metadata_refresh_rate) {
    return metadata_refresh_rate;
  }

  return active_refresh_rate_;
}

uint32_t DisplayBuiltIn::CalculateMetaDataRefreshRate() {
  LayerStack *layer_stack = disp_layer_stack_->stack;
  uint32_t metadata_refresh_rate = 0;
  if (!layer_stack->flags.use_metadata_refresh_rate) {
    return 0;
  }

  uint32_t max_refresh_rate = 0;
  uint32_t min_refresh_rate = 0;
  GetRefreshRateRange(&min_refresh_rate, &max_refresh_rate);

  for (uint i = 0; i < layer_stack->layers.size(); i++) {
    auto layer = layer_stack->layers.at(i);
    if (layer->flags.has_metadata_refresh_rate && layer->frame_rate > metadata_refresh_rate) {
      metadata_refresh_rate = SanitizeRefreshRate(layer->frame_rate, max_refresh_rate,
                                                  min_refresh_rate);
    }
  }
  return metadata_refresh_rate;
}

uint32_t DisplayBuiltIn::SanitizeRefreshRate(uint32_t req_refresh_rate, uint32_t max_refresh_rate,
                                             uint32_t min_refresh_rate) {
  uint32_t refresh_rate = req_refresh_rate;

  if (refresh_rate < min_refresh_rate) {
    // Pick the next multiple of request which is within the range
    refresh_rate = (((min_refresh_rate / refresh_rate) +
                     ((min_refresh_rate % refresh_rate) ? 1 : 0)) * refresh_rate);
  }

  if (refresh_rate > max_refresh_rate) {
    refresh_rate = max_refresh_rate;
  }

  return refresh_rate;
}

DisplayError DisplayBuiltIn::SetDemuraState(int state, int demura_idx) {
  int ret = 0;
  DisplayError error = kErrorNone;

  if (!comp_manager_->GetDemuraStatus()) {
    DLOGI("Demura status is not ready, failed to set state %d", state);
    return kErrorUndefined;
  }

  DLOGI("Setting the Demura state %d, config = %d", state, demura_idx);
  if (!demura_intended_ && state) {
    if (!demura_allowed_) {
      // Validate demura license again in case failed during boot up
      error = ValidateDemuraLicense();
      if (error) {
        DLOGE("Failed to validate Demura license, error %d", error);
      }
    }
    if (demura_allowed_) {
      // Parse parsers for case: apply T0 config without reboot
      std::shared_ptr<DemuraTnValidatorIntf> validator_intf =
          pf_factory_->CreateDemuraTnValidatorIntf();
      if (!validator_intf) {
        DLOGW("Failed to get DemuraTnValidatorIntf");
        return kErrorUndefined;
      }
      int ret = validator_intf->Init();
      if (ret) {
        DLOGW("Failed to init DemuraTnValidatorIntf, ret %d", ret);
        return kErrorResources;
      }

      // Config demura double buffer codebook flags
      GenericPayload flags_pl;
      bool *flags_ptr = nullptr;
      ret = flags_pl.CreatePayload<bool>(flags_ptr);
      if (ret) {
        DLOGE("Failed to create the payload for flags_ptr. Error:%d", ret);
        return kErrorResources;
      }
      *flags_ptr = double_buffer_codebook_supported_;
      ret = validator_intf->SetParameter(kDemuraTnValidatorDoubleBufferCodebookFlags, flags_pl);
      if (ret) {
        DLOGE("Failed to Set double buffer codebook flags, ret %d", ret);
        return kErrorResources;
      }

      GenericPayload input_payload;
      ret = validator_intf->SetParameter(kDemuraTnValidatorCleanupFiles, input_payload);
      if (ret) {
        DLOGW("Failed to Set DemuraTnValidatorCleanupFiles, ret %d", ret);
        return kErrorResources;
      }

      error = SendPanelIdToParserManager();
      if (error) {
        DLOGE("Failed to setup parser manager, error %d", error);
        return error;
      }
      DLOGI("Start Demura feature now");
      if ((error = SetupDemuraT0(demura_idx)) != kErrorNone) {
        DLOGE("Failed to enable Demura dynamically, error = %d", error);
        return error;
      }
      demura_dynamic_enabled_ = true;
      // Disable Partial Update for one frame.
      DisablePartialUpdateOneFrameInternal();
    } else {
      DLOGI("Demura is not allowed by license");
    }
    return kErrorNone;
  }

  if (state && !comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
    if (SetupCorrectionLayer() != kErrorNone) {
      DLOGE("Unable to setup Demura layer on Display %d", display_id_);
      return kErrorUndefined;
    }

    ret = SetDemuraIntfStatus(true, demura_idx);
    if (ret) {
      DLOGE("Failed to set demura status to true, ret = %d", ret);
      return kErrorUndefined;
    }
    comp_manager_->SetDemuraStatusForDisplay(display_id_, true);
    demura_dynamic_enabled_ = true;
  } else if (!state && comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
    ret = SetDemuraIntfStatus(false);
    if (ret) {
      DLOGE("Failed to set demura status to false, ret = %d", ret);
      return kErrorUndefined;
    }
    comp_manager_->SetDemuraStatusForDisplay(display_id_, false);
    demura_dynamic_enabled_ = false;
    demura_layer_.clear();
  }

  // Disable Partial Update for one frame.
  DisablePartialUpdateOneFrameInternal();

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraConfig(int demura_idx) {
  ClientLock lock(disp_mutex_);
  int ret = 0;
  GenericPayload pl;
  uConfigIdx *idx = nullptr;

  if (!demura_intended_ || !demura_dynamic_enabled_) {
    DLOGW("Demura is not enabled");
    return kErrorNone;
  }

  DLOGI("Setting the Demura Config, config = %d", demura_idx);
  if (demura_idx < kDemuraDefaultIdx) {
    DLOGE("Invalid demura config index");
    return kErrorParameters;
  }

  if (demura_idx == demura_current_idx_) {
    return kErrorNone;
  }

  // Check the override feature
  if (demuratn_override_feature_ == kFeatureDAC) {
    DLOGE("Cannot switch demura config when override feature is DAC");
    return kErrorUndefined;
  }

  // Update demura config
  if ((ret = pl.CreatePayload<uConfigIdx>(idx))) {
    DLOGE("Failed to create payload for enable, error = %d", ret);
    return kErrorUndefined;
  }

  idx->modeinfo = demura_idx;
  if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, pl))) {
    DLOGE("Failed to update demura config, error = %d", ret);
    return kErrorUndefined;
  }

  if (SetupCorrectionLayer() != kErrorNone) {
    DLOGE("Unable to setup Demura layer on Display %d", display_id_);
    return kErrorUndefined;
  }

  if (SetDemuraIntfStatus(true, demura_idx)) {
    DLOGE("Failed to set Demura Status on Display %d", display_id_);
    return kErrorUndefined;
  }

  DLOGV("Demura config updated to config index %d", demura_idx);
  HandleSelfRefresh();

  // disable partial update for one frame
  DisablePartialUpdateOneFrame();

  return kErrorNone;
}

void DisplayBuiltIn::GetDRMDisplayToken(uint32_t core_id, sde_drm::DRMDisplayToken *token) {
  dpu_core_mux_->GetDRMDisplayToken(core_id, token);
}

bool DisplayBuiltIn::IsPrimaryDisplay() {
  return DisplayBase::IsPrimaryDisplay();
}

DisplayError DisplayBuiltIn::GetPanelBrightnessBasePath(std::string *base_path) {
  return dpu_core_mux_->GetPanelBrightnessBasePath(base_path);
}

bool DisplayBuiltIn::IsCacV2Supported() {
  for (auto &res_info : hw_resource_info_) {
    if ((res_info.cac_version != kCacVersion2) && (res_info.cac_version != kCacVersionLoopback)) {
      return false;
    }
  }

  return true;
}

DisplayError DisplayBuiltIn::PerformCacConfig(CacConfig config, bool enable) {
  ClientLock lock(disp_mutex_);

  if (!IsCacV2Supported()) {
    return kErrorNotSupported;
  }

  DLOGV_IF(kTagDisplay, "CAC enable: %d Config:: k0r: %f k1r: %f k0b: %f k1b: %f pixel_pitch: %f"
           "normalization: %f mid_le_y_offset: %d mid_le_x_offset: %d mid_re_y_offset: %d"
           " mid_re_x_offset: %d skip_inc: %d", enable, config.k0r, config.k1r, config.k0b,
           config.k1b, config.pixel_pitch, config.normalization, config.mid_le_y_offset,
           config.mid_le_x_offset, config.mid_re_y_offset, config.mid_re_x_offset, config.skip_inc);

  enable_cac_ = enable;
  cac_config_ = config;
  validated_ = false;
  event_handler_->Refresh();

  return kErrorNone;
}

DisplayError
DisplayBuiltIn::PanelOprInfo(const std::string &client_name, bool enable,
                             SdmDisplayCbInterface<PanelOprPayload> *cb_intf) {
  return event_proxy_info_.PanelOprInfo(client_name, enable, cb_intf);
}

DisplayError DisplayBuiltIn::SetPaHistCollection(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PaHistCollectionPayload> *cb_intf) {
  return event_proxy_info_.SetPaHistCollection(client_name, enable, cb_intf);
}

DisplayError DisplayBuiltIn::GetPaHistBins(std::array<uint32_t, HIST_BIN_SIZE> *buf) {
  return event_proxy_info_.GetPaHistBins(buf);
}

DisplayError DisplayBuiltIn::PanelBacklightInfo(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PanelBacklightPayload> *cb_intf) {
  return event_proxy_info_.PanelBacklightInfo(client_name, enable, cb_intf);
}

#ifndef TARGET_INCLUDES_NEO
DisplayError DisplayBuiltIn::EnableCopr(bool en) {
  DisplayError ret = kErrorNone;

  DLOGI("%s COPR", en ? "Enable" : "Disable");
  ret = event_proxy_info_.EnableCopr("copr_test", en, &copr_info_);
  if (ret) {
    DLOGW("Failed to enable COPR ret %d", ret);
  } else {
    event_handler_->Refresh();
    copr_enabled_ = en;
  }

  return ret;
}

DisplayError DisplayBuiltIn::GetCoprStats(std::vector<int> *stats) {
  DisplayError ret = kErrorNone;

  if (!copr_enabled_) {
    DLOGW("COPR is not enabled");
    return kErrorResources;
  }

  ret = copr_info_.GetStats(stats);
  if (ret)
    DLOGE("Failed to get COPR stats ret %d", ret);
  return ret;
}
#endif

DisplayError DisplayBuiltIn::GetScalerCount(uint32_t *scaler_count) {
  int enable_ai_scaler = 0;
  Debug::Get()->GetProperty(ENABLE_AI_SCALER_PROP, &enable_ai_scaler);
  *scaler_count = 0;

  if (client_ctx_.hw_panel_info.is_primary_panel && enable_ai_scaler) {
    for (auto &res_info : hw_resource_info_) {
      *scaler_count = std::max(*scaler_count, res_info.hw_ai_scaler_count);
    }
  } else {
    for (auto &res_info : hw_resource_info_) {
      *scaler_count = std::max(*scaler_count, res_info.hw_dest_scalar_info.count);
    }
  }

  return kErrorNone;
}

DisplayError EventProxyInfo::Init(const std::string &panel_name, DisplayInterface *intf,
                                  DynLib &extension_lib, PanelFeaturePropertyIntf *prop_intf) {
  std::lock_guard<std::mutex> guard(lock_);

  if (!intf || !prop_intf) {
    DLOGE("Invalid display_intf %pK prop_intf %pK", intf, prop_intf);
    return kErrorParameters;
  }

  if (event_proxy_intf_.get()) {
    DLOGV("Event proxy interface is already created");
    return kErrorNone;
  }

  typedef DispEventProxyFactIntf *(*GetDispEventProxyFactFunc)();
  GetDispEventProxyFactFunc get_disp_event_proxy_fact_func;

  if (!extension_lib.Sym(
          "GetDispEventProxyFactIntf",
          reinterpret_cast<void **>(&get_disp_event_proxy_fact_func))) {
    DLOGW("Fail to retrieve GetDispEventProxyFactIntf from %s",
          EXTENSION_LIBRARY_NAME);
    return kErrorUndefined;
  }

  DispEventProxyFactIntf *factory_intf = get_disp_event_proxy_fact_func();
  if (!factory_intf) {
    DLOGW("Failed to get display event proxy factory interface");
    return kErrorUndefined;
  }

  std::shared_ptr<DisplayEventProxyIntf> proxy_intf =
      factory_intf->CreateDispEventProxyIntf(panel_name, intf, prop_intf);
  if (!proxy_intf) {
    DLOGW("Failed to create display event proxy interface");
    return kErrorMemory;
  }

  int ret = proxy_intf->Init();
  if (ret) {
    DLOGW("Failed to initialize event proxy interface, ret %d", ret);
    return kErrorUndefined;
  }

  event_proxy_intf_ = proxy_intf;
  return kErrorNone;
}

DisplayError EventProxyInfo::Deinit() {
  std::lock_guard<std::mutex> guard(lock_);
  if (event_proxy_intf_) {
    event_proxy_intf_->Deinit();
    event_proxy_intf_.reset();
    event_proxy_intf_ = nullptr;
  }
  return kErrorNone;
}

DisplayError
EventProxyInfo::PanelOprInfo(const std::string &client_name, bool enable,
                             SdmDisplayCbInterface<PanelOprPayload> *cb_intf) {
  if (!event_proxy_intf_.get()) {
    DLOGW("Event proxy intf is not available");
    return kErrorParameters;
  }

  PanelOprInfoParam *opr_info = nullptr;
  GenericPayload payload;
  int ret = payload.CreatePayload(opr_info);
  if (ret || !opr_info) {
    DLOGE("Failed to create payload for OPR info, ret %d", ret);
    return kErrorParameters;
  }

  opr_info->name = client_name;
  opr_info->enable = enable;
  opr_info->cb_intf = cb_intf;

  ret = event_proxy_intf_->SetParameter(kSetPanelOprInfoEnable, payload);
  if (ret) {
    DLOGE("Failed to set panel Opr info enablement, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

#ifndef TARGET_INCLUDES_NEO
DisplayError EventProxyInfo::EnableCopr(const std::string &client_name, bool enable,
                                        SdmDisplayCbInterface<CoprEventPayload> *cb_intf) {
  if (!event_proxy_intf_.get()) {
    DLOGW("Event proxy intf is not available");
    return kErrorParameters;
  }

  CoprParam *copr_info = nullptr;
  GenericPayload payload;
  int ret = payload.CreatePayload(copr_info);
  if (ret || !copr_info) {
    DLOGE("Failed to create payload for COPR info, ret %d", ret);
    return kErrorParameters;
  }

  copr_info->name = client_name;
  copr_info->enable = enable;
  copr_info->cb_intf = cb_intf;

  ret = event_proxy_intf_->SetParameter(kSetCoprEnable, payload);
  if (ret) {
    DLOGE("Failed to set Copr info enablement, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

DisplayError CoprInfo::GetStats(std::vector<int32_t> *stats) {
  std::lock_guard<std::mutex> guard(lock_);

  if (!stats) {
    DLOGE("Invalid input parameter stats %pK", stats);
    return kErrorUndefined;
  }

  *stats = copr_stats_;
  return kErrorNone;
}

int CoprInfo::Notify(const CoprEventPayload &payload) {
  std::lock_guard<std::mutex> guard(lock_);
  struct drm_msm_copr_status *copr_info =
      reinterpret_cast<struct drm_msm_copr_status *>(payload.payload);

  copr_stats_.clear();
  for (auto i = 0; i < AIQE_COPR_STATUS_LEN; i++) {
    copr_stats_.push_back(copr_info->status[i]);
  }

  return 0;
}
#endif

DisplayError EventProxyInfo::SetPaHistCollection(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PaHistCollectionPayload> *cb_intf) {
  if (!event_proxy_intf_.get()) {
    DLOGW("Event proxy intf is not available");
    return kErrorParameters;
  }

  PaHistCollectionParam *param = nullptr;
  GenericPayload payload;
  int ret = payload.CreatePayload(param);
  if (ret || !param) {
    DLOGE("Failed to create payload for pa hist, ret %d", ret);
    return kErrorParameters;
  }

  param->name = client_name;
  param->enable = enable;
  param->cb_intf = cb_intf;

  ret = event_proxy_intf_->SetParameter(kSetPaHistCollection, payload);
  if (ret) {
    DLOGE("Failed to set pa hist enablement, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

DisplayError EventProxyInfo::GetPaHistBins(std::array<uint32_t, HIST_BIN_SIZE> *buf) {
  PaHistBinsParam *param = nullptr;
  GenericPayload payload;

  if (!event_proxy_intf_.get()) {
    DLOGW("Event proxy intf is not available");
    return kErrorParameters;
  }

  if (!buf) {
    DLOGE("Invalid pa hist bins buf");
    return kErrorParameters;
  }

  int ret = payload.CreatePayload(param);
  if (ret || !param) {
    DLOGE("Failed to create payload for pa hist bins, ret %d", ret);
    return kErrorParameters;
  }

  param->buf = buf;
  ret = event_proxy_intf_->GetParameter(kGetPaHistBins, &payload);
  if (ret) {
    DLOGE("Failed to get pa hist bins, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

DisplayError EventProxyInfo::PanelBacklightInfo(
    const std::string &client_name, bool enable,
    SdmDisplayCbInterface<PanelBacklightPayload> *cb_intf) {
  if (!event_proxy_intf_.get()) {
    DLOGW("Event proxy intf is not available");
    return kErrorParameters;
  }

  PanelBacklightInfoParam *backlight_info = nullptr;
  GenericPayload payload;
  int ret = payload.CreatePayload(backlight_info);
  if (ret || !backlight_info) {
    DLOGE("Failed to create payload for backlight info, ret %d", ret);
    return kErrorParameters;
  }

  backlight_info->name = client_name;
  backlight_info->enable = enable;
  backlight_info->cb_intf = cb_intf;

  ret = event_proxy_intf_->SetParameter(kSetPanelBLInfoEnable, payload);
  if (ret) {
    DLOGE("Failed to set panel backlight info enablement, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

#ifndef TARGET_INCLUDES_NEO
DisplayError DisplayBuiltIn::SetSsrcMode(const std::string &mode) {
  DisplayError ret = kErrorNotSupported;

  if (ssrc_feature_enabled_ && ssrc_feature_interface_) {
    std::string *mode_str = nullptr;
    GenericPayload payload;
    int rc = payload.CreatePayload(mode_str);
    if (rc) {
      DLOGE("Unable to create mode string payload! RC - %d", rc);
    } else {
      *mode_str = mode;
      rc = ssrc_feature_interface_->SetParameter(aiqe::kSsrcFeatureModeId, payload);
      if (rc) {
        DLOGE("Mode rejected by SSRC feature interface. RC - %d", rc);
      } else {
        ret = kErrorNone;
      }
    }
  }

  needs_validate_ = true;
  return ret;
}
#endif

DisplayError DisplayBuiltIn::SetAvrStepFpsState(uint32_t index, bool enable) {
  ClientLock lock(disp_mutex_);

  if (enable && (client_ctx_.hw_panel_info.mode == kModeVideo)) {
    if (!client_ctx_.hw_panel_info.qsync_support || (qsync_mode_ != kQSyncModeContinuous)) {
      DLOGW("AVR Step fps switch is not supported.");
      return kErrorNotSupported;
    }
  }

  uint32_t active_index = 0;
  uint32_t to_avr_step_fps = hw_intf_->GetAVRStep(index);
  dpu_core_mux_->GetActiveConfig(&active_index);
  uint32_t curr_avr_step_fps = hw_intf_->GetAVRStep(active_index);

  if (to_avr_step_fps == curr_avr_step_fps) {
    DLOGI("AVR Step fps already set in requested fps %d", to_avr_step_fps);
    return kErrorNone;
  }

  needs_avr_update_.set(kUpdateAVRStepFpsFlag);
  validated_ = false;
  event_handler_->Refresh();

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetAVRStepState(bool enable) {
  ClientLock lock(disp_mutex_);

  if (enable && (client_ctx_.hw_panel_info.mode == kModeVideo)) {
    if (!client_ctx_.hw_panel_info.qsync_support || (qsync_mode_ == kQSyncModeNone)) {
      DLOGW("AVR Step feature is not supported without QSync on VID mode");
      return kErrorNotSupported;
    }
  }

  if (avr_step_enabled_ == enable) {
    DLOGI("AVR Step already set in requested state %d", enable);
    return kErrorNone;
  }

  avr_step_enabled_ = enable;
  needs_avr_update_.set(kUpdateAVRStepFlag);
  validated_ = false;
  event_handler_->Refresh();
  DLOGI("AVR Step state set to %d successfully", avr_step_enabled_);

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetVRRState(bool state) {
  if (!hw_intf_->IsVRRSupported()) {
    return kErrorNotSupported;
  }

  uint32_t active_index = 0;
  dpu_core_mux_->GetActiveConfig(&active_index);
  avr_step_ = hw_intf_->GetAVRStep(active_index);
  if (avr_step_ != 0) {
    DLOGI("Set VRR state %d in config %d", state, active_index);
    SetQSyncMode(state ? kQSyncModeContinuous : kQSyncModeNone);
    DisplayError error = SetAVRStepState(state);
    if (error != kErrorNone) {
      return error;
    }
  }

  vrr_enabled_ = state;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetABCState(bool state) {
  if (!demura_) {
    DLOGI("ABC feature intf is not available");
    return kErrorUndefined;
  }

  DLOGV("Setting the ABC State to %d", state);

  int ret = 0;
  bool current_abc_state = comp_manager_->GetDemuraStatusForDisplay(display_id_);

  // Check if current ABC state and coming ABC state is same
  if (state == current_abc_state) {
    DLOGI("ABC Feature state already set to %s", state ? "true" : "false");
    return kErrorNone;
  }

  // If current ABC state is disable and coming ABC state is enable
  if (state && !current_abc_state) {
    if (demura_->Init() != 0) {
      DLOGE("Unable to initialize ABC on Display %d-%d", display_id_, display_type_);
      return kErrorUndefined;
    }
  }

  // Enable or Disable ABC
  if (SetDemuraIntfStatus(state)) {
    DLOGE("Failed to set demura status to %s on Display %d, ret = %d", state ? "true" : "false", display_id_, ret);
    return kErrorUndefined;
  }

  // Update dispay abc state for current display
  comp_manager_->SetDemuraStatusForDisplay(display_id_, state);
  abc_enabled_ = state;

  if (abc_enabled_ && (SetABCMode("normal_on_udc_off") != kErrorNone)) {
    DLOGE("Failed to set mode to normal_on_udc_off");
    return kErrorUndefined;
  }

  needs_validate_ = true;
  // Disable Partial Update for one frame.
  DisablePartialUpdateOneFrameInternal();
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetABCReconfig() {
  if (!demura_) {
    DLOGI("ABC feature intf is not available");
    return kErrorUndefined;
  }

  if (!abc_prop_) {
    DLOGI("ABC feature is not enabled");
    return kErrorUndefined;
  }

  if (!comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
    return kErrorUndefined;
  }

  int ret = 0;
  GenericPayload pl;
  bool *b = nullptr;
  ret = pl.CreatePayload<bool>(b);
  if (ret) {
    DLOGE("Failed to create kDemuraFeatureParamPendingReconfig payload");
    return kErrorUndefined;
  }

  // Setting reconfig as true
  *b = true;
  ret = demura_->SetParameter(kDemuraFeatureParamPendingReconfig, pl);
  if (ret) {
    DLOGE("Failed to set reconfig parameter for ABC %d", ret);
    return kErrorUndefined;
  }

  if (SetDemuraIntfStatus(true)) {
    DLOGE("Failed to set ABC Status on Display %d", display_id_);
    return kErrorUndefined;
  }

  DisplayError error = ExportABCFiles();
  if (error) {
    DLOGE("Failed to export ABC files, error %d", error);
    return kErrorUndefined;
  }

  needs_validate_ = true;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetABCMode(const string &mode_name) {
  if (!demura_) {
    DLOGI("ABC feature intf is not available");
    return kErrorUndefined;
  }

  if (mode_name.empty()) {
    DLOGI("mode name is empty");
    return kErrorUndefined;
  }

  int ret = 0;
  GenericPayload config_pl;

  DemuraFeatureParamConfigIdx<std::string> *config_mode_name = nullptr;
  if ((ret = config_pl.CreatePayload(config_mode_name))) {
    DLOGE("Failed to create payload for config_mode_name, error = %d", ret);
    return kErrorUndefined;
  }

  // Setting the mode name
  config_mode_name->modeinfo = mode_name;
  if ((ret = demura_->SetParameter(kDemuraFeatureParamConfigIdx, config_pl))) {
    DLOGE("Failed to set Config Idx, error = %d", ret);
    return kErrorUndefined;
  }

  // Set up ABC correction layer for updated mode name
  if (SetupCorrectionLayer() != kErrorNone) {
    DLOGE("Unable to setup ABC layer on Display %d", display_id_);
    return kErrorUndefined;
  }

#ifdef TRUSTED_VM
  if (abc_brightness_level_ >= 0) {
    int ret = 0;
    GenericPayload pl;
    uint32_t *brightness_level = nullptr;
    ret = pl.CreatePayload<uint32_t>(brightness_level);
    if (ret) {
      DLOGE("Failed to create kDemuraFeatureParamUpdateBrightness payload");
      return kErrorUndefined;
    }

    // Set the ABC feature with new brightness level and updated mode name
    *brightness_level = abc_brightness_level_;
    ret = demura_->SetParameter(kDemuraFeatureParamUpdateBrightness, pl);
    if (ret) {
      DLOGE("Failed to set brightness level for ABC feature %d", ret);
      return kErrorUndefined;
    }

    abc_brightness_level_ = -1;
    return kErrorNone;
  }
#endif

  // Set the ABC feature with updated mode name
  GenericPayload pl;
  bool *enable_ptr = nullptr;
  if ((ret = pl.CreatePayload<bool>(enable_ptr))) {
    DLOGE("Failed to create payload for enable, error = %d", ret);
    return kErrorUndefined;
  } else {
    *enable_ptr = true;
    if ((ret = demura_->SetParameter(kDemuraFeatureParamActive, pl))) {
      DLOGE("Failed to set Active, error = %d", ret);
      return kErrorUndefined;
    }
  }

  needs_validate_ = true;
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetAIScalerMode(uint32_t mode_id) {
  DisplayError ret = kErrorParameters;

  if (IsPrimaryDisplay()) {
    ret = comp_manager_->SetAIScalerMode(mode_id);
  }

  if (ret) {
    DLOGE("Unable to set AI Scaler mode ID. error: %d", ret);
  }

  needs_validate_ = true;
  return ret;
}

DisplayError DisplayBuiltIn::SetPanelFeatureConfig(int32_t type, void *data) {
  DisplayError ret = kErrorNone;

  switch (type) {
    case kTypeDemuraTnCWBSamplingPeriod:
      ret = SetDemuraTnCWBSamplingPeriod(data);
      break;
    case kTypeDemuraTnEventsCtrl:
      ret = SetDemuraTnEventsCtrl(data);
      break;
    case kTypeDemuraTnUserCtrl:
      ret = SetDemuraTnUserCtrl(data);
      break;
    case kTypeDeleteDemuraConfig:
      ret = CleanupDemuraConfig(data, kDeleteDemuraConfig);
      break;
    case kTypeDeleteDemuraTnConfig:
      ret = CleanupDemuraConfig(data, kDeleteDemuraTnConfig);
      break;
    case kTypeTriggerDemuraOemPlugIn:
      ret = TriggerDemuraOemPlugIn(data);
      break;
    case kTypeReloadDemuraCalibFiles:
      ret = ReloadDemuraCalibFiles(data);
      break;
    case kTypeDemuraDisplayEventsCtrl:
      ret = SetDemuraDisplayEventsCtrl(data);
      break;
    case kTypeQueryDemuraTnInfo:
      ret = QueryDemuraTnInfo(data);
      break;
    case kTypeDemuraTnBatchId:
      ret = SetDemuraTnBatchId(data);
      break;
    case kTypeDemuraTnAodHandlerCtrl:
      ret = SetDemuraTnAodHandlerCtrl(data);
      break;
    case kTypeDemuraTnAgingSurfTransfer:
      ret = SetDemuraTnAgingSurfTransfer(data);
      break;
    case kTypeSwitchToDAC:
      ret = SwitchToDAC(data);
      break;
    default:
      DLOGE("Invalid type %d", type);
      ret = kErrorParameters;
      break;
  }
  return ret;
}

DisplayError DisplayBuiltIn::GetPanelFeatureConfig(int32_t type, void *data, uint32_t data_size) {
  DisplayError ret = kErrorNone;

  if (!data || !data_size) {
    DLOGE("Invalid input data %pK, data size %d", data, data_size);
    return kErrorParameters;
  }

  switch (type) {
    case kTypeGetDemuraTnAgingValue:
      ret = GetDemuraTnAgingValue(data, data_size);
      break;
    default:
      DLOGE("Invalid type %d", type);
      ret = kErrorParameters;
      break;
  }

  return ret;
}

DisplayError DisplayBuiltIn::GetDemuraTnAgingValue(void *data, uint32_t data_size) {
  int ret = 0;
  GenericPayload payload = {};
  DemuraTnAgingValues *aging_values = nullptr;

  if (!demuratn_ || !data) {
    DLOGE("Invalid demuratn_ %pK, data %pK", demuratn_.get(), data);
    return kErrorUndefined;
  }

  ret = payload.CreatePayload<DemuraTnAgingValues>(aging_values);
  if (ret || aging_values == nullptr) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }

  ret = demuratn_->GetParameter(kDemuraTnCoreUvmParamAgingValues, &payload);
  if (ret) {
    DLOGE("Get aging values failed ret %d", ret);
    return kErrorUndefined;
  }

  char *output = reinterpret_cast<char *>(data);
  snprintf(output, data_size, "%.2f %.2f %.2f", aging_values->value[0], aging_values->value[1],
           aging_values->value[2]);
  DLOGI("Query aging values: %s", output);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnCWBSamplingPeriod(void *data) {
  int ret = 0;
  int *period_ptr = nullptr;
  GenericPayload payload = {};

  if (!data || !demuratn_ || !demuratn_enabled_) {
    DLOGE("Data %pK demuratn_ %pK demuratn_enabled_ %d", data, demuratn_.get(), demuratn_enabled_);
    return kErrorUndefined;
  }

  ret = payload.CreatePayload<int>(period_ptr);
  if (ret) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }
  *period_ptr = *(reinterpret_cast<int *>(data));

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamCWBSamplingPeriod, payload);
  if (ret) {
    DLOGE("Set CWB sampling period failed ret %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set CWB sampling period %d success", *period_ptr);

  return kErrorNone;
}

DisplayError DisplayBuiltIn::ExportDemuraFiles() {
#if !defined(SDM_UNIT_TESTING) && !defined(TRUSTED_VM)
  int ret = 0;
  const std::string kConfigFilePath = "/mnt/vendor/persist/display/";
  std::vector<std::string> configs = {"demura_config_", "demura_publickey_", "demura_signature_"};

  if (!panel_id_) {
    DLOGE("Invalid panel id %lx", panel_id_);
    return kErrorUndefined;
  }

  if (!vm_file_xfer_intf_) {
    DLOGE("Invalid xfer client intf");
    return kErrorUndefined;
  }

  std::vector<std::string> filenames;
  for (const std::string &config : configs) {
    std::stringstream file_path;
    file_path << kConfigFilePath << config << std::setfill('0') << std::setw(16) << std::hex
              << panel_id_;

    std::string filename = file_path.str();
    std::ifstream file(filename);
    if (!file.good()) {
      DLOGE("File does not exist or is not readable: %s", filename.c_str());
      return kErrorUndefined;
    }

    filenames.push_back(filename);
  }

  GenericPayload s_in;
  VMFileXferStoreInput *s_ip = nullptr;
  ret = s_in.CreatePayload<VMFileXferStoreInput>(s_ip);
  if (ret || s_ip == nullptr) {
    DLOGE("Failed to create input payload error = %d", ret);
    return kErrorUndefined;
  }

  for (const std::string &filename : filenames) {
    s_ip->local_file_path = filename;
    ret = vm_file_xfer_intf_->SetParameter(kVMFileTransferParamsStore, s_in);
    if (ret) {
      DLOGE("Failed to store config file: %s", s_ip->local_file_path.c_str());
      return kErrorUndefined;
    }
  }
#endif

  return kErrorNone;
}

DisplayError DisplayBuiltIn::ExportABCFiles() {
  if (abc_tvm_enabled_ && demura_) {
    GenericPayload in;
    int ret = demura_->SetParameter(kDemuraFeatureParamExportFiles, in);
    if (ret != 0) {
      DLOGW("Failed to export ABC files");
      return kErrorUndefined;
    }
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::StartTvmServices() {
  if (!abc_tvm_enabled_ && !demura_enable_) {
    return kErrorNone;
  }

#ifndef SDM_UNIT_TESTING
  sleep(5);  // sleep 5 seconds to make sure persist is mounted on TVM
#endif

  int ret = CreateServiceManager();
  if (ret) {
    DLOGE("Failed to create servicemanager, ret %d", ret);
    return kErrorUndefined;
  }

  ret = StartVmFileServiceAndExportFiles();
  if (ret) {
    DLOGE("Failed to start VmFileTransferService and export files, ret %d", ret);
    return kErrorUndefined;
  }

  if (demuratn_factory_) {
    demuratn_cleanup_intf_ = demuratn_factory_->CreateDemuraTnCleanupIntf(buffer_allocator_);
    if (!demuratn_cleanup_intf_) {
      DLOGW("Failed to create DemuraTnCleanupIntf");
    } else {
      int ret = demuratn_cleanup_intf_->Init();
      if (ret) {
        DLOGE("Failed to init DemuraTnCleanupIntf, ret %d", ret);
        demuratn_cleanup_intf_.reset();
      }
    }
  }

  return kErrorNone;
}

int DisplayBuiltIn::CreateServiceManager() {
  // Create service manager
  if (!service_manager_intf_) {
    if (!pf_factory_) {
      DLOGE("Invalid panel feature factory");
      return -EINVAL;
    }

    service_manager_intf_ = pf_factory_->CreateTvmServiceManager();
    if (!service_manager_intf_) {
      DLOGE("Failed to get Tvm Service Manager intf");
      return -EINVAL;
    }

    if (service_manager_intf_->Init() != 0) {
      DLOGE("Failed to init Tvm Service Manager intf");
      service_manager_intf_.reset();
      service_manager_intf_ = nullptr;
      return -EINVAL;
    }
  }

  // Create factory extn
  if (!factory_extn_) {
    if (!extension_lib_) {
      DLOGE("Invalid lib %s", EXTENSION_LIBRARY_NAME);
      return -EINVAL;
    }

    typedef VmFileXferClientFactIntfExtn *(*GetVmFileXferClientFactIntfExtn)();
    GetVmFileXferClientFactIntfExtn get_vm_file_fact_intf_extn_ptr = nullptr;
    if (!extension_lib_.Sym(GET_VM_FILE_XFER_CLIENT_FACT_INTF_EXTN,
                            reinterpret_cast<void **>(&get_vm_file_fact_intf_extn_ptr))) {
      DLOGE("Unable to load symbols, error = %s", extension_lib_.Error());
      return -EINVAL;
    }

    factory_extn_ = get_vm_file_fact_intf_extn_ptr();
    if (!factory_extn_) {
      DLOGE("Failed to get VmFileXferClient factory extn");
      return -EINVAL;
    }
  }

  return 0;
}

int DisplayBuiltIn::StartVmFileServiceAndExportFiles() {
  int ret = 0;
  DisplayError error = kErrorNone;

  if (!service_manager_intf_) {
    DLOGE("Invalid service manager");
    return -EINVAL;
  }

  // Start VmFileTransferService
  GenericPayload in;
  ret = service_manager_intf_->SetParameter(kStartVmFileTransferService, in);
  if (ret) {
    DLOGE("Failed to start VmFileTransferService, ret %d", ret);
    return ret;
  } else {
    DLOGI("Started kStartVmFileTransferService");
  }

  error = ExportABCFiles();
  if (error) {
    DLOGE("Failed to export ABC files, error %d", error);
    return -EINVAL;
  }

  if (!factory_extn_) {
    DLOGE("Invalid factory extn");
    return -EINVAL;
  }

  // CreateVMFileXferClient
  vm_file_xfer_intf_ = factory_extn_->CreateVmFileXferClient(
      static_cast<SdmDisplayCbInterface<TvmServiceCbEvent> *>(this), buffer_allocator_);
  if (!vm_file_xfer_intf_) {
    DLOGE("Failed to create VmFileXferClient");
    return -EINVAL;
  }

  ret = vm_file_xfer_intf_->Init();
  if (ret) {
    DLOGE("Failed to init VmFileXferClient ret %d", ret);
    vm_file_xfer_intf_->Deinit();
    vm_file_xfer_intf_.reset();
    vm_file_xfer_intf_ = nullptr;
    return ret;
  } else {
    DLOGI("Created VmFileXferClient");
  }

  // Export files
  if (demura_enable_) {
    error = ExportDemuraFiles();
    if (error) {
      DLOGE("Failed to export demura files, error %d", error);
    }
  }

  return ret;
}

DisplayError DisplayBuiltIn::HandleDemuraScreenRefresh() {
  if (demura_intended_ && comp_manager_->GetDemuraStatusForDisplay(display_id_)) {
    if (!demura_) {
      DLOGE("demura_ is nullptr");
      return kErrorParameters;
    }

    GenericPayload pl;
    int32_t *need_screen_refresh = nullptr;
    int rc = 0;
    if ((rc = pl.CreatePayload<int32_t>(need_screen_refresh))) {
      DLOGE("Failed to create payload for need_screen_refresh, error = %d", rc);
      return kErrorParameters;
    }

    rc = demura_->GetParameter(kDemuraFeatureParamNeedScreenRefresh, &pl);
    if (rc) {
      DLOGE("Failed to get need screen refresh, error %d", rc);
      return kErrorParameters;
    }

    if (*need_screen_refresh) {
      event_handler_->Refresh();
    }
  }

  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnEventsCtrl(void *data) {
  int ret = 0;
  bool *ctrl_ptr = nullptr;
  GenericPayload payload = {};

  if (!data || !demuratn_ || !demuratn_enabled_) {
    DLOGE("Data %pK demuratn_ %pK demuratn_enabled_ %d", data, demuratn_.get(), demuratn_enabled_);
    return kErrorUndefined;
  }

  ret = payload.CreatePayload<bool>(ctrl_ptr);
  if (ret) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }
  *ctrl_ptr = *(reinterpret_cast<bool *>(data));

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamEventsCtrl, payload);
  if (ret) {
    DLOGE("Set events ctrl failed ret %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set events ctrl %d success", *ctrl_ptr);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnUserCtrl(void *data) {
  DisplayError ret = kErrorNone;
  if (!data) {
    DLOGE("Invalid parameters");
    return kErrorParameters;
  }

  bool user_ctrl = *(reinterpret_cast<uint32_t *>(data)) ? true : false;
  if (demuratn_user_disabled_ == false) {
    ret = EnableDemuraTn(false);
    if (ret != kErrorNone) {
      return ret;
    }
  }
  demuratn_user_disabled_ = true;

  int error = UpdateDemuraTnUserCtrl(user_ctrl);
  if (error) {
    DLOGE("Failed to update demura user ctrl, error = %d", error);
    return kErrorUndefined;
  }

  return ret;
}

DisplayError DisplayBuiltIn::CleanupDemuraConfig(void *data, DemuraTnCleanupType type) {
  int ret = 0;
  if (!data) {
    DLOGE("Invalid parameters");
    return kErrorParameters;
  }
  if (!demuratn_cleanup_intf_) {
    DLOGW("Demuratn cleanup intf is not created");
    return kErrorNotSupported;
  }

  uint64_t panel_id = *(reinterpret_cast<uint64_t *>(data));

  DemuraTnCleanupDeleteConfigInput *input = nullptr;
  GenericPayload payload;
  ret = payload.CreatePayload(input);
  if (ret || !input) {
    DLOGE("Failed to create payload for DeleteConfigInput, ret %d", ret);
    return kErrorParameters;
  }

  input->panel_id = panel_id;
  if (type == kDeleteDemuraConfig) {
    input->delete_t0 = true;
  }
  if (type == kDeleteDemuraTnConfig) {
    input->delete_tn = true;
  }

  ret = demuratn_cleanup_intf_->SetParameter(kDemuraTnCleanupDeleteConfig, payload);
  if (ret) {
    DLOGE("Failed to SetParameter, param %d, ret %d", kDemuraTnCleanupDeleteConfig, ret);
    return kErrorParameters;
  }

  return kErrorNone;
}

bool DisplayBuiltIn::GetDemuraTnUserCtrl() {
  std::stringstream ss_id;
  ss_id << "_" << std::setfill('0') << std::setw(16) << std::hex << panel_id_;
  std::string filename = kDemuraTnUserCtrlFile + ss_id.str();
  std::ifstream in(filename, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  in.seekg(0, in.end);
  int length = in.tellg();
  in.seekg(0, in.beg);
  std::string file_data(length, '\0');
  in.read(&file_data[0], length);

  if (static_cast<int>(file_data.size()) != length) {
    DLOGE("Couldn't read the whole file");
    return false;
  }

  auto pos = file_data.find("demuratn_permanent_disabled=true");
  if (pos != std::string::npos) {
    return true;
  }

  return false;
}

int DisplayBuiltIn::UpdateDemuraTnUserCtrl(bool user_ctrl) {
  int ret = 0;
  std::stringstream ss_id;
  ss_id << "_" << std::setfill('0') << std::setw(16) << std::hex << panel_id_;
  std::string filename = kDemuraTnUserCtrlFile + ss_id.str();
  std::ofstream out(filename, std::ios::binary | std::ios::trunc);
  if (out.fail()) {
    DLOGW("Failed to open the file %s %s", kDemuraTnUserCtrlFile.c_str(), strerror(errno));
    return -ENOENT;
  }

  std::string value = user_ctrl ? "true" : "false";
  std::string prefix = "demuratn_permanent_disabled=";
  out << prefix << value << '\n';
  return ret;
}

DisplayError DisplayBuiltIn::TriggerDemuraOemPlugIn(void *data) {
  (void)data;
  int ret = 0;
  int current_brightness_level = 0;
  struct DemuraBacklightInfo *demura_bl_info = nullptr;
  GenericPayload payload = {};

  if (!demura_intended_ || !demura_dynamic_enabled_) {
    DLOGW("Demura is not enabled");
    return kErrorNone;
  }

  ret = payload.CreatePayload<struct DemuraBacklightInfo>(demura_bl_info);
  if (ret) {
    DLOGE("Failed to create payload");
    return kErrorUndefined;
  }

  // Get current brightness level
  DisplayError error = dpu_core_mux_->GetPanelBrightness(&current_brightness_level);
  if (error != kErrorNone) {
    DLOGE("Failed to get current brightness level, error %d", error);
    return error;
  }

  // Fill demura backlight info
  demura_bl_info->os_brightness = current_brightness_level;
  demura_bl_info->os_brightness_max = client_ctx_.hw_panel_info.panel_max_brightness;

  // Call demura backlight event for trigger oem plugin
  ret = demura_->SetParameter(kDemuraFeatureParamBacklightEvent, payload);
  if (ret) {
    DLOGE("Failed to set backlight event");
    return kErrorUndefined;
  }

  // Trigger refresh, new config take effect
  event_handler_->Refresh();

  DLOGI("Trigger demura oem plugin success");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnBatchId(void *data) {
  int ret = 0;
  GenericPayload payload = {};
  uint32_t *batch_id = nullptr;

  if (!data || !demuratn_) {
    DLOGE("Data %pK demuratn_ %pK", data, demuratn_.get());
    return kErrorUndefined;
  }

  ret = payload.CreatePayload<uint32_t>(batch_id);
  if (ret) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }
  *batch_id = *(reinterpret_cast<uint32_t *>(data));

  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamBatchId, payload);
  if (ret) {
    DLOGE("Set batch id failed ret %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set batch id %d success", *batch_id);
  return kErrorNone;
}

DisplayError DisplayBuiltIn::ReloadDemuraCalibFiles(void *data) {
  (void)data;
  int ret = 0;

  // OEM will reload demura calib files during device suspend
  // the new calib files will take effect after device resume
  if (state_ != kStateOff) {
    DLOGW("Not supported in display state %d", state_);
    return kErrorNotSupported;
  }

  if (!demura_intended_) {
    DLOGW("Demura is not enabled");
    return kErrorNone;
  }

  if (!pm_intf_) {
    DLOGE("Invalid parser manager intf");
    return kErrorUndefined;
  }

  // Re-parse all demura files
  GenericPayload pl = {};
  if ((ret = pm_intf_->SetParameter(kDemuraParserManagerReparseDemuraFiles, pl))) {
    DLOGE("Failed to reparse demura calib files, ret %d", ret);
    return kErrorResources;
  }

  // Reconfig demura with new data
  bool *b = nullptr;
  GenericPayload reconfig_pl = {};
  if ((ret = reconfig_pl.CreatePayload<bool>(b))) {
    DLOGE("Failed to create payload, ret %d", ret);
    return kErrorUndefined;
  }
  *b = true;
  if ((ret = demura_->SetParameter(kDemuraFeatureParamPendingReconfig, reconfig_pl))) {
    DLOGE("Failed to set reconfig parameter for demura %d", ret);
    return kErrorUndefined;
  }

  demura_calib_files_reloaded_ = true;
  DLOGI("Reload demura calib files success");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraDisplayEventsCtrl(void *data) {
  (void)data;
  int ret = 0;

  if (!demura_intended_ || !demura_dynamic_enabled_) {
    DLOGW("Demura is not enabled");
    return kErrorNone;
  }

  GenericPayload pl = {};
  if ((ret = demura_->SetParameter(kDemuraFeatureParamDispEventState, pl))) {
    DLOGE("Failed to set DispEventState for demura %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set demura disply event state success");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::QueryDemuraTnInfo(void *data) {
  (void)data;
  int ret = 0;

  if (!demuratn_) {
    DLOGE("Not supported, demuratn intf is null");
    return kErrorNotSupported;
  }

  GenericPayload payload = {};
  ret = demuratn_->GetParameter(kDemuraTnCoreUvmParamQueryInfo, &payload);
  if (ret) {
    DLOGE("Failed to query information from Tn %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Query demuraTn infomation done");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnAodHandlerCtrl(void *data) {
  (void)data;

  if (!demuratn_ || !demuratn_enabled_) {
    DLOGE("Demuratn_ %pK demuratn_enabled_ %d", demuratn_.get(), demuratn_enabled_);
    return kErrorUndefined;
  }

  GenericPayload payload = {};
  int ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamAodHandlerCtrl, payload);
  if (ret) {
    DLOGE("Set aod handler ctrl failed ret %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set aod handler ctrl done");
  return kErrorNone;
}

DisplayError DisplayBuiltIn::SetDemuraTnAgingSurfTransfer(void *data) {
  (void)data;
  if (demuratn_enabled_) {
    DLOGE("Pls disable demuraTn temporarily before aging surface transfer");
    return kErrorUndefined;
  }

  if (!demuratn_) {
    DLOGE("Demuratn_ is %pK", demuratn_.get());
    return kErrorUndefined;
  }

  GenericPayload payload = {};
  int ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamAgingSurfTransfer, payload);
  if (ret) {
    DLOGE("Set demuraTn aging surface transfer failed ret %d", ret);
    return kErrorUndefined;
  }

  DLOGI("Set demuraTn aging surface transfer done");
  return kErrorNone;
}

int DisplayBuiltIn::HandleTvmServiceEvent(const TvmServiceCbEvent &event) {
  DLOGI("Handle TVM service event %d", event);
  if (event == kVmFileTransferServiceDead) {
    if (vm_file_xfer_intf_) {
      vm_file_xfer_intf_->Deinit();
      vm_file_xfer_intf_.reset();
      vm_file_xfer_intf_ = nullptr;
    }

    int ret = StartVmFileServiceAndExportFiles();
    if (ret) {
      DLOGE("Failed to restart VmFileTransferService and export files, ret %d", ret);
      return ret;
    }
  } else {
    DLOGW("Unsupported event type %d", event);
  }
  return 0;
}

int DisplayBuiltIn::Notify(const TvmServiceCbEvent &event) {
  std::thread([=] { DisplayBuiltIn::HandleTvmServiceEvent(event); }).detach();
  return 0;
}

DisplayError DisplayBuiltIn::SwitchToDAC(void *data) {
  int ret = 0;

  (void)data;
  if (!demuratn_) {
    DLOGE("Invalid demuratn_ %pK", demuratn_.get());
    return kErrorUndefined;
  }

  if (demuratn_override_feature_ == kFeatureDAC) {
    DLOGI("Current feature is already DAC, nothing to do");
    return kErrorNone;
  }

  // Demura: Switch to default config
  if (demura_current_idx_ != kDemuraDefaultIdx) {
    ret = SetDemuraConfig(kDemuraDefaultIdx);
    if (ret) {
      DLOGW("Failed to switch to Demura config %d, curr config %d", kDemuraDefaultIdx,
            demura_current_idx_);
    } else {
      DLOGI("Switched to Demura default config %d from config %d", kDemuraDefaultIdx,
            demura_current_idx_);
      demura_current_idx_ = kDemuraDefaultIdx;
    }
  }

  // DemuraTn: Switch to DAC: enable recalibration
  GenericPayload payload;
  DemuraFeatureType *feature = nullptr;
  ret = payload.CreatePayload<DemuraFeatureType>(feature);
  if (ret) {
    DLOGE("Failed to create the payload, ret %d", ret);
    return kErrorUndefined;
  }
  *feature = kFeatureDAC;
  ret = demuratn_->SetParameter(kDemuraTnCoreUvmParamOverrideFeature, payload);
  if (ret) {
    DLOGE("Failed to set override feature to DAC, ret %d", ret);
    return kErrorUndefined;
  }

  // Demura: clear multi-config parsers
  ClearDemuraMultiCfgParsers();
  demuratn_override_feature_ = kFeatureDAC;
  DLOGI("Switch to DAC done");
  return kErrorNone;
}

void DisplayBuiltIn::ClearDemuraMultiCfgParsers() {
  int ret = 0;
  PanelIdsInfo *panel_ids_info = nullptr;
  GenericPayload in;

  if (!pm_intf_ || !panel_id_) {
    DLOGW("Invalid parser manager intf, panel id 0x%lx", panel_id_);
    return;
  }

  ret = in.CreatePayload<PanelIdsInfo>(panel_ids_info);
  if (ret || !panel_ids_info) {
    DLOGW("Failed to create payload for panel id info, ret %d", ret);
    return;
  }

  panel_ids_info->panel_ids.push_back(panel_id_);
  ret = pm_intf_->SetParameter(kDemuraParserManagerReleaseMultiCfgParsers, in);
  if (ret) {
    DLOGW("Failed to release DUC multi-config parsers for base panel_id 0x%lx", panel_id_);
  } else {
    DLOGI("Released DUC multi-config parsers for base panel_id 0x%lx successfully", panel_id_);
  }
}

DisplayError DisplayBuiltIn::DisableDemuraForHandOff() {
  if (!prop_intf_) {
    DLOGE("prop_intf_ is nullptr");
    return kErrorParameters;
  }

  PanelFeaturePropertyInfo payload = {};
  payload.prop_id = kPanelFeatureDemuraInitCfg;

  int ret = prop_intf_->SetPanelFeature(payload);
  if (ret) {
    DLOGE("Failed to SetPanelFeature, ret %d", ret);
    return kErrorUndefined;
  }

  return kErrorNone;
}

void DisplayBuiltIn::SetPrivacyRegions() {
  if (!privacy_region_mgr_) {
    return;
  }

  std::vector<PrivacyRegion> regions = {};
  DisplayError ret = privacy_region_mgr_->ConfigurePrivacyRegions(
      disp_layer_stack_, client_ctx_, mixer_resolution_updated_, &regions);
  if (ret == kErrorNeedsCommit) {
    disp_layer_stack_->stack_info.common_info.updates_mask.set(kUpdatePrivacyRegions);
    for (int i = 0; i < hw_resource_info_.size(); i++) {
      uint32_t core_id = hw_resource_info_[i].core_id;
      disp_layer_stack_->info.at(core_id).privacy_regions_ = regions;
    }
  }
}

}  // namespace sdm
