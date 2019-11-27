/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <map>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#define LOG_TAG "APM_Test"
#include <Serializer.h>
#include <android-base/file.h>
#include <media/AudioPolicy.h>
#include <media/PatchBuilder.h>
#include <media/RecordingActivityTracker.h>
#include <utils/Log.h>
#include <utils/Vector.h>

#include "AudioPolicyInterface.h"
#include "AudioPolicyTestClient.h"
#include "AudioPolicyTestManager.h"

using namespace android;

TEST(AudioPolicyManagerTestInit, EngineFailure) {
    AudioPolicyTestClient client;
    AudioPolicyTestManager manager(&client);
    manager.getConfig().setDefault();
    manager.getConfig().setEngineLibraryNameSuffix("non-existent");
    ASSERT_EQ(NO_INIT, manager.initialize());
    ASSERT_EQ(NO_INIT, manager.initCheck());
}

TEST(AudioPolicyManagerTestInit, ClientFailure) {
    AudioPolicyTestClient client;
    AudioPolicyTestManager manager(&client);
    manager.getConfig().setDefault();
    // Since the default client fails to open anything,
    // APM should indicate that the initialization didn't succeed.
    ASSERT_EQ(NO_INIT, manager.initialize());
    ASSERT_EQ(NO_INIT, manager.initCheck());
}


class AudioPolicyManagerTestClient : public AudioPolicyTestClient {
  public:
    // AudioPolicyClientInterface implementation
    audio_module_handle_t loadHwModule(const char* /*name*/) override {
        return mNextModuleHandle++;
    }

    status_t openOutput(audio_module_handle_t module,
                        audio_io_handle_t* output,
                        audio_config_t* /*config*/,
                        audio_devices_t* /*devices*/,
                        const String8& /*address*/,
                        uint32_t* /*latencyMs*/,
                        audio_output_flags_t /*flags*/) override {
        if (module >= mNextModuleHandle) {
            ALOGE("%s: Module handle %d has not been allocated yet (next is %d)",
                    __func__, module, mNextModuleHandle);
            return BAD_VALUE;
        }
        *output = mNextIoHandle++;
        return NO_ERROR;
    }

    status_t openInput(audio_module_handle_t module,
                       audio_io_handle_t* input,
                       audio_config_t* /*config*/,
                       audio_devices_t* /*device*/,
                       const String8& /*address*/,
                       audio_source_t /*source*/,
                       audio_input_flags_t /*flags*/) override {
        if (module >= mNextModuleHandle) {
            ALOGE("%s: Module handle %d has not been allocated yet (next is %d)",
                    __func__, module, mNextModuleHandle);
            return BAD_VALUE;
        }
        *input = mNextIoHandle++;
        return NO_ERROR;
    }

    status_t createAudioPatch(const struct audio_patch* patch,
                              audio_patch_handle_t* handle,
                              int /*delayMs*/) override {
        *handle = mNextPatchHandle++;
        mActivePatches.insert(std::make_pair(*handle, *patch));
        return NO_ERROR;
    }

    status_t releaseAudioPatch(audio_patch_handle_t handle,
                               int /*delayMs*/) override {
        if (mActivePatches.erase(handle) != 1) {
            if (handle >= mNextPatchHandle) {
                ALOGE("%s: Patch handle %d has not been allocated yet (next is %d)",
                        __func__, handle, mNextPatchHandle);
            } else {
                ALOGE("%s: Attempt to release patch %d twice", __func__, handle);
            }
            return BAD_VALUE;
        }
        return NO_ERROR;
    }

    // Helper methods for tests
    size_t getActivePatchesCount() const { return mActivePatches.size(); }

    const struct audio_patch* getLastAddedPatch() const {
        if (mActivePatches.empty()) {
            return nullptr;
        }
        auto it = --mActivePatches.end();
        return &it->second;
    };

  private:
    audio_module_handle_t mNextModuleHandle = AUDIO_MODULE_HANDLE_NONE + 1;
    audio_io_handle_t mNextIoHandle = AUDIO_IO_HANDLE_NONE + 1;
    audio_patch_handle_t mNextPatchHandle = AUDIO_PATCH_HANDLE_NONE + 1;
    std::map<audio_patch_handle_t, struct audio_patch> mActivePatches;
};

class PatchCountCheck {
  public:
    explicit PatchCountCheck(AudioPolicyManagerTestClient *client)
            : mClient{client},
              mInitialCount{mClient->getActivePatchesCount()} {}
    int deltaFromSnapshot() const {
        size_t currentCount = mClient->getActivePatchesCount();
        if (mInitialCount <= currentCount) {
            return currentCount - mInitialCount;
        } else {
            return -(static_cast<int>(mInitialCount - currentCount));
        }
    }
  private:
    const AudioPolicyManagerTestClient *mClient;
    const size_t mInitialCount;
};

class AudioPolicyManagerTest : public testing::Test {
  protected:
    void SetUp() override;
    void TearDown() override;
    virtual void SetUpConfig(AudioPolicyConfig *config) { (void)config; }

    void dumpToLog();
    void getOutputForAttr(
            audio_port_handle_t *selectedDeviceId,
            audio_format_t format,
            int channelMask,
            int sampleRate,
            audio_output_flags_t flags = AUDIO_OUTPUT_FLAG_NONE,
            audio_port_handle_t *portId = nullptr,
            audio_attributes_t attr = {});
    void getInputForAttr(
            const audio_attributes_t &attr,
            audio_unique_id_t riid,
            audio_port_handle_t *selectedDeviceId,
            audio_format_t format,
            int channelMask,
            int sampleRate,
            audio_input_flags_t flags = AUDIO_INPUT_FLAG_NONE,
            audio_port_handle_t *portId = nullptr);
    PatchCountCheck snapshotPatchCount() { return PatchCountCheck(mClient.get()); }

    void findDevicePort(audio_port_role_t role, audio_devices_t deviceType,
            const std::string &address, audio_port &foundPort);

    std::unique_ptr<AudioPolicyManagerTestClient> mClient;
    std::unique_ptr<AudioPolicyTestManager> mManager;
};

void AudioPolicyManagerTest::SetUp() {
    mClient.reset(new AudioPolicyManagerTestClient);
    mManager.reset(new AudioPolicyTestManager(mClient.get()));
    mManager->getConfig().setDefault();
    SetUpConfig(&mManager->getConfig());  // Subclasses may want to customize the config.
    ASSERT_EQ(NO_ERROR, mManager->initialize());
    ASSERT_EQ(NO_ERROR, mManager->initCheck());
}

void AudioPolicyManagerTest::TearDown() {
    mManager.reset();
    mClient.reset();
}

void AudioPolicyManagerTest::dumpToLog() {
    int pipefd[2];
    ASSERT_NE(-1, pipe(pipefd));
    pid_t cpid = fork();
    ASSERT_NE(-1, cpid);
    if (cpid == 0) {
        // Child process reads from the pipe and logs.
        close(pipefd[1]);
        std::string line;
        char buf;
        while (read(pipefd[0], &buf, sizeof(buf)) > 0) {
            if (buf != '\n') {
                line += buf;
            } else {
                ALOGI("%s", line.c_str());
                line = "";
            }
        }
        if (!line.empty()) ALOGI("%s", line.c_str());
        close(pipefd[0]);
        _exit(EXIT_SUCCESS);
    } else {
        // Parent does the dump and checks the status code.
        close(pipefd[0]);
        ASSERT_EQ(NO_ERROR, mManager->dump(pipefd[1]));
        close(pipefd[1]);
        wait(NULL);  // Wait for the child to exit.
    }
}

void AudioPolicyManagerTest::getOutputForAttr(
        audio_port_handle_t *selectedDeviceId,
        audio_format_t format,
        int channelMask,
        int sampleRate,
        audio_output_flags_t flags,
        audio_port_handle_t *portId,
        audio_attributes_t attr) {
    audio_io_handle_t output = AUDIO_PORT_HANDLE_NONE;
    audio_stream_type_t stream = AUDIO_STREAM_DEFAULT;
    audio_config_t config = AUDIO_CONFIG_INITIALIZER;
    config.sample_rate = sampleRate;
    config.channel_mask = channelMask;
    config.format = format;
    *selectedDeviceId = AUDIO_PORT_HANDLE_NONE;
    audio_port_handle_t localPortId;
    if (!portId) portId = &localPortId;
    *portId = AUDIO_PORT_HANDLE_NONE;
    ASSERT_EQ(OK, mManager->getOutputForAttr(
                    &attr, &output, AUDIO_SESSION_NONE, &stream, 0 /*uid*/, &config, &flags,
                    selectedDeviceId, portId, {}));
    ASSERT_NE(AUDIO_PORT_HANDLE_NONE, *portId);
}

void AudioPolicyManagerTest::getInputForAttr(
        const audio_attributes_t &attr,
        audio_unique_id_t riid,
        audio_port_handle_t *selectedDeviceId,
        audio_format_t format,
        int channelMask,
        int sampleRate,
        audio_input_flags_t flags,
        audio_port_handle_t *portId) {
    audio_io_handle_t input = AUDIO_PORT_HANDLE_NONE;
    audio_config_base_t config = AUDIO_CONFIG_BASE_INITIALIZER;
    config.sample_rate = sampleRate;
    config.channel_mask = channelMask;
    config.format = format;
    *selectedDeviceId = AUDIO_PORT_HANDLE_NONE;
    audio_port_handle_t localPortId;
    if (!portId) portId = &localPortId;
    *portId = AUDIO_PORT_HANDLE_NONE;
    AudioPolicyInterface::input_type_t inputType;
    ASSERT_EQ(OK, mManager->getInputForAttr(
            &attr, &input, riid, AUDIO_SESSION_NONE, 0 /*uid*/, &config, flags,
            selectedDeviceId, &inputType, portId));
    ASSERT_NE(AUDIO_PORT_HANDLE_NONE, *portId);
}

void AudioPolicyManagerTest::findDevicePort(audio_port_role_t role,
        audio_devices_t deviceType, const std::string &address, audio_port &foundPort) {
    uint32_t numPorts = 0;
    uint32_t generation1;
    status_t ret;

    ret = mManager->listAudioPorts(role, AUDIO_PORT_TYPE_DEVICE, &numPorts, nullptr, &generation1);
    ASSERT_EQ(NO_ERROR, ret);

    uint32_t generation2;
    struct audio_port ports[numPorts];
    ret = mManager->listAudioPorts(role, AUDIO_PORT_TYPE_DEVICE, &numPorts, ports, &generation2);
    ASSERT_EQ(NO_ERROR, ret);
    ASSERT_EQ(generation1, generation2);

    for (const auto &port : ports) {
        if (port.role == role && port.ext.device.type == deviceType &&
                (strncmp(port.ext.device.address, address.c_str(),
                         AUDIO_DEVICE_MAX_ADDRESS_LEN) == 0)) {
            foundPort = port;
            return;
        }
    }
    GTEST_FAIL();
}


TEST_F(AudioPolicyManagerTest, InitSuccess) {
    // SetUp must finish with no assertions.
}

TEST_F(AudioPolicyManagerTest, Dump) {
    dumpToLog();
}

TEST_F(AudioPolicyManagerTest, CreateAudioPatchFailure) {
    audio_patch patch{};
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;
    const PatchCountCheck patchCount = snapshotPatchCount();
    ASSERT_EQ(BAD_VALUE, mManager->createAudioPatch(nullptr, &handle, 0));
    ASSERT_EQ(BAD_VALUE, mManager->createAudioPatch(&patch, nullptr, 0));
    ASSERT_EQ(BAD_VALUE, mManager->createAudioPatch(&patch, &handle, 0));
    patch.num_sources = AUDIO_PATCH_PORTS_MAX + 1;
    patch.num_sinks = 1;
    ASSERT_EQ(BAD_VALUE, mManager->createAudioPatch(&patch, &handle, 0));
    patch.num_sources = 1;
    patch.num_sinks = AUDIO_PATCH_PORTS_MAX + 1;
    ASSERT_EQ(BAD_VALUE, mManager->createAudioPatch(&patch, &handle, 0));
    patch.num_sources = 2;
    patch.num_sinks = 1;
    ASSERT_EQ(INVALID_OPERATION, mManager->createAudioPatch(&patch, &handle, 0));
    patch = {};
    patch.num_sources = 1;
    patch.sources[0].role = AUDIO_PORT_ROLE_SINK;
    patch.num_sinks = 1;
    patch.sinks[0].role = AUDIO_PORT_ROLE_SINK;
    ASSERT_EQ(INVALID_OPERATION, mManager->createAudioPatch(&patch, &handle, 0));
    patch = {};
    patch.num_sources = 1;
    patch.sources[0].role = AUDIO_PORT_ROLE_SOURCE;
    patch.num_sinks = 1;
    patch.sinks[0].role = AUDIO_PORT_ROLE_SOURCE;
    ASSERT_EQ(INVALID_OPERATION, mManager->createAudioPatch(&patch, &handle, 0));
    // Verify that the handle is left unchanged.
    ASSERT_EQ(AUDIO_PATCH_HANDLE_NONE, handle);
    ASSERT_EQ(0, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTest, CreateAudioPatchFromMix) {
    audio_patch_handle_t handle = AUDIO_PATCH_HANDLE_NONE;
    uid_t uid = 42;
    const PatchCountCheck patchCount = snapshotPatchCount();
    ASSERT_FALSE(mManager->getConfig().getAvailableInputDevices().isEmpty());
    PatchBuilder patchBuilder;
    patchBuilder.addSource(mManager->getConfig().getAvailableInputDevices()[0]).
            addSink(mManager->getConfig().getDefaultOutputDevice());
    ASSERT_EQ(NO_ERROR, mManager->createAudioPatch(patchBuilder.patch(), &handle, uid));
    ASSERT_NE(AUDIO_PATCH_HANDLE_NONE, handle);
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
}

// TODO: Add patch creation tests that involve already existing patch

class AudioPolicyManagerTestMsd : public AudioPolicyManagerTest {
  protected:
    void SetUpConfig(AudioPolicyConfig *config) override;
    void TearDown() override;

    sp<DeviceDescriptor> mMsdOutputDevice;
    sp<DeviceDescriptor> mMsdInputDevice;
};

void AudioPolicyManagerTestMsd::SetUpConfig(AudioPolicyConfig *config) {
    // TODO: Consider using Serializer to load part of the config from a string.
    mMsdOutputDevice = new DeviceDescriptor(AUDIO_DEVICE_OUT_BUS);
    sp<AudioProfile> pcmOutputProfile = new AudioProfile(
            AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO, 48000);
    sp<AudioProfile> ac3OutputProfile = new AudioProfile(
            AUDIO_FORMAT_AC3, AUDIO_CHANNEL_OUT_5POINT1, 48000);
    mMsdOutputDevice->addAudioProfile(pcmOutputProfile);
    mMsdOutputDevice->addAudioProfile(ac3OutputProfile);
    mMsdInputDevice = new DeviceDescriptor(AUDIO_DEVICE_IN_BUS);
    // Match output profile from AudioPolicyConfig::setDefault.
    sp<AudioProfile> pcmInputProfile = new AudioProfile(
            AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_IN_STEREO, 44100);
    mMsdInputDevice->addAudioProfile(pcmInputProfile);
    config->addAvailableDevice(mMsdOutputDevice);
    config->addAvailableDevice(mMsdInputDevice);

    sp<HwModule> msdModule = new HwModule(AUDIO_HARDWARE_MODULE_ID_MSD, 2 /*halVersionMajor*/);
    HwModuleCollection modules = config->getHwModules();
    modules.add(msdModule);
    config->setHwModules(modules);
    mMsdOutputDevice->attach(msdModule);
    mMsdInputDevice->attach(msdModule);

    sp<OutputProfile> msdOutputProfile = new OutputProfile(String8("msd input"));
    msdOutputProfile->addAudioProfile(pcmOutputProfile);
    msdOutputProfile->addSupportedDevice(mMsdOutputDevice);
    msdModule->addOutputProfile(msdOutputProfile);
    sp<OutputProfile> msdCompressedOutputProfile =
            new OutputProfile(String8("msd compressed input"));
    msdCompressedOutputProfile->addAudioProfile(ac3OutputProfile);
    msdCompressedOutputProfile->setFlags(
            AUDIO_OUTPUT_FLAG_DIRECT | AUDIO_OUTPUT_FLAG_COMPRESS_OFFLOAD |
            AUDIO_OUTPUT_FLAG_NON_BLOCKING);
    msdCompressedOutputProfile->addSupportedDevice(mMsdOutputDevice);
    msdModule->addOutputProfile(msdCompressedOutputProfile);

    sp<InputProfile> msdInputProfile = new InputProfile(String8("msd output"));
    msdInputProfile->addAudioProfile(pcmInputProfile);
    msdInputProfile->addSupportedDevice(mMsdInputDevice);
    msdModule->addInputProfile(msdInputProfile);

    // Add a profile with another encoding to the default device to test routing
    // of streams that are not supported by MSD.
    sp<AudioProfile> dtsOutputProfile = new AudioProfile(
            AUDIO_FORMAT_DTS, AUDIO_CHANNEL_OUT_5POINT1, 48000);
    config->getDefaultOutputDevice()->addAudioProfile(dtsOutputProfile);
    sp<OutputProfile> primaryEncodedOutputProfile = new OutputProfile(String8("encoded"));
    primaryEncodedOutputProfile->addAudioProfile(dtsOutputProfile);
    primaryEncodedOutputProfile->setFlags(AUDIO_OUTPUT_FLAG_DIRECT);
    primaryEncodedOutputProfile->addSupportedDevice(config->getDefaultOutputDevice());
    config->getHwModules().getModuleFromName(AUDIO_HARDWARE_MODULE_ID_PRIMARY)->
            addOutputProfile(primaryEncodedOutputProfile);
}

void AudioPolicyManagerTestMsd::TearDown() {
    mMsdOutputDevice.clear();
    mMsdInputDevice.clear();
    AudioPolicyManagerTest::TearDown();
}

TEST_F(AudioPolicyManagerTestMsd, InitSuccess) {
    ASSERT_TRUE(mMsdOutputDevice);
    ASSERT_TRUE(mMsdInputDevice);
}

TEST_F(AudioPolicyManagerTestMsd, Dump) {
    dumpToLog();
}

TEST_F(AudioPolicyManagerTestMsd, PatchCreationOnSetForceUse) {
    const PatchCountCheck patchCount = snapshotPatchCount();
    mManager->setForceUse(AUDIO_POLICY_FORCE_FOR_ENCODED_SURROUND,
            AUDIO_POLICY_FORCE_ENCODED_SURROUND_ALWAYS);
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTestMsd, GetOutputForAttrEncodedRoutesToMsd) {
    const PatchCountCheck patchCount = snapshotPatchCount();
    audio_port_handle_t selectedDeviceId;
    getOutputForAttr(&selectedDeviceId,
            AUDIO_FORMAT_AC3, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT);
    ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTestMsd, GetOutputForAttrPcmRoutesToMsd) {
    const PatchCountCheck patchCount = snapshotPatchCount();
    audio_port_handle_t selectedDeviceId;
    getOutputForAttr(&selectedDeviceId,
            AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO, 48000);
    ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTestMsd, GetOutputForAttrEncodedPlusPcmRoutesToMsd) {
    const PatchCountCheck patchCount = snapshotPatchCount();
    audio_port_handle_t selectedDeviceId;
    getOutputForAttr(&selectedDeviceId,
            AUDIO_FORMAT_AC3, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT);
    ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
    getOutputForAttr(&selectedDeviceId,
            AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO, 48000);
    ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
    ASSERT_EQ(1, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTestMsd, GetOutputForAttrUnsupportedFormatBypassesMsd) {
    const PatchCountCheck patchCount = snapshotPatchCount();
    audio_port_handle_t selectedDeviceId;
    getOutputForAttr(&selectedDeviceId,
            AUDIO_FORMAT_DTS, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT);
    ASSERT_NE(selectedDeviceId, mMsdOutputDevice->getId());
    ASSERT_EQ(0, patchCount.deltaFromSnapshot());
}

TEST_F(AudioPolicyManagerTestMsd, GetOutputForAttrFormatSwitching) {
    // Switch between formats that are supported and not supported by MSD.
    {
        const PatchCountCheck patchCount = snapshotPatchCount();
        audio_port_handle_t selectedDeviceId, portId;
        getOutputForAttr(&selectedDeviceId,
                AUDIO_FORMAT_AC3, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT,
                &portId);
        ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
        ASSERT_EQ(1, patchCount.deltaFromSnapshot());
        mManager->releaseOutput(portId);
        ASSERT_EQ(1, patchCount.deltaFromSnapshot());
    }
    {
        const PatchCountCheck patchCount = snapshotPatchCount();
        audio_port_handle_t selectedDeviceId, portId;
        getOutputForAttr(&selectedDeviceId,
                AUDIO_FORMAT_DTS, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT,
                &portId);
        ASSERT_NE(selectedDeviceId, mMsdOutputDevice->getId());
        ASSERT_EQ(-1, patchCount.deltaFromSnapshot());
        mManager->releaseOutput(portId);
        ASSERT_EQ(0, patchCount.deltaFromSnapshot());
    }
    {
        const PatchCountCheck patchCount = snapshotPatchCount();
        audio_port_handle_t selectedDeviceId;
        getOutputForAttr(&selectedDeviceId,
                AUDIO_FORMAT_AC3, AUDIO_CHANNEL_OUT_5POINT1, 48000, AUDIO_OUTPUT_FLAG_DIRECT);
        ASSERT_EQ(selectedDeviceId, mMsdOutputDevice->getId());
        ASSERT_EQ(0, patchCount.deltaFromSnapshot());
    }
}

using PolicyMixTuple = std::tuple<audio_usage_t, audio_source_t, uint32_t>;

class AudioPolicyManagerTestDynamicPolicy : public AudioPolicyManagerTest {
protected:
    void SetUp() override;
    void TearDown() override;

    status_t addPolicyMix(int mixType, int mixFlag, audio_devices_t deviceType,
            std::string mixAddress, const audio_config_t& audioConfig,
            const std::vector<PolicyMixTuple>& rules);
    void clearPolicyMix();
    void resetManager(const std::string& configFileName);
    audio_port_handle_t getDeviceIdFromPatch(const struct audio_patch* patch);

    Vector<AudioMix> mAudioMixes;
    const std::string mExecutableDir = base::GetExecutableDirectory();
    const std::string mConfig = mExecutableDir + "/test_audio_policy_configuration.xml";
    const std::string mPrimaryOnlyConfig = mExecutableDir +
            "/test_audio_policy_primary_only_configuration.xml";
    const std::string mMixAddress = "remote_submix_media";
};

void AudioPolicyManagerTestDynamicPolicy::SetUp() {
    // Override Setup function to use configuration file to do initialization.
    mClient.reset(new AudioPolicyManagerTestClient);
    resetManager(mConfig);
}

void AudioPolicyManagerTestDynamicPolicy::TearDown() {
    mManager->unregisterPolicyMixes(mAudioMixes);
    AudioPolicyManagerTest::TearDown();
}

status_t AudioPolicyManagerTestDynamicPolicy::addPolicyMix(int mixType, int mixFlag,
        audio_devices_t deviceType, std::string mixAddress, const audio_config_t& audioConfig,
        const std::vector<PolicyMixTuple>& rules) {
    Vector<AudioMixMatchCriterion> myMixMatchCriteria;

    for(const auto &rule: rules) {
        myMixMatchCriteria.add(AudioMixMatchCriterion(
                std::get<0>(rule), std::get<1>(rule), std::get<2>(rule)));
    }

    AudioMix myAudioMix(myMixMatchCriteria, mixType, audioConfig, mixFlag,
            String8(mixAddress.c_str()), 0);
    myAudioMix.mDeviceType = deviceType;
    // Clear mAudioMix before add new one to make sure we don't add already exist mixes.
    mAudioMixes.clear();
    mAudioMixes.add(myAudioMix);

    // As the policy mixes registration may fail at some case,
    // caller need to check the returned status.
    status_t ret = mManager->registerPolicyMixes(mAudioMixes);
    return ret;
}

void AudioPolicyManagerTestDynamicPolicy::clearPolicyMix() {
    if (mManager != nullptr) {
        mManager->unregisterPolicyMixes(mAudioMixes);
    }
    mAudioMixes.clear();
}

void AudioPolicyManagerTestDynamicPolicy::resetManager(const std::string& configFileName) {
    clearPolicyMix();
    mManager.reset(new AudioPolicyTestManager(mClient.get()));
    status_t status = deserializeAudioPolicyFile(configFileName.c_str(), &mManager->getConfig());
    ASSERT_EQ(NO_ERROR, status);
    ASSERT_EQ(NO_ERROR, mManager->initialize());
    ASSERT_EQ(NO_ERROR, mManager->initCheck());
}

audio_port_handle_t AudioPolicyManagerTestDynamicPolicy::getDeviceIdFromPatch(
        const struct audio_patch* patch) {
    // The logic here is the same as the one in AudioIoDescriptor.
    // Note this function is aim to get routed device id for test.
    // In that case, device to device patch is not expected here.
    if (patch->num_sources != 0 && patch->num_sinks != 0) {
        if (patch->sources[0].type == AUDIO_PORT_TYPE_MIX) {
            return patch->sinks[0].id;
        } else {
            return patch->sources[0].id;
        }
    }
    return AUDIO_PORT_HANDLE_NONE;
}

TEST_F(AudioPolicyManagerTestDynamicPolicy, InitSuccess) {
    // SetUp must finish with no assertions.
}

TEST_F(AudioPolicyManagerTestDynamicPolicy, Dump) {
    dumpToLog();
}

TEST_F(AudioPolicyManagerTestDynamicPolicy, RegisterPolicyMixes) {
    status_t ret;
    audio_config_t audioConfig = AUDIO_CONFIG_INITIALIZER;

    // Only capture of playback is allowed in LOOP_BACK &RENDER mode
    ret = addPolicyMix(MIX_TYPE_RECORDERS, MIX_ROUTE_FLAG_LOOP_BACK_AND_RENDER,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(INVALID_OPERATION, ret);

    // Fail due to the device is already connected.
    clearPolicyMix();
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(INVALID_OPERATION, ret);

    // The first time to register policy mixes with valid parameter should succeed.
    clearPolicyMix();
    audioConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    audioConfig.format = AUDIO_FORMAT_PCM_16_BIT;
    audioConfig.sample_rate = 48000;
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, mMixAddress, audioConfig,
            std::vector<PolicyMixTuple>());
    ASSERT_EQ(NO_ERROR, ret);
    // Registering the same policy mixes should fail.
    ret = mManager->registerPolicyMixes(mAudioMixes);
    ASSERT_EQ(INVALID_OPERATION, ret);

    // Registration should fail due to not module for remote submix found.
    resetManager(mPrimaryOnlyConfig);
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(INVALID_OPERATION, ret);

    // Registration should fail due to device not found.
    // Note that earpiece is not present in the test configuration file.
    // This will need to be updated if earpiece is added in the test configuration file.
    resetManager(mConfig);
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_RENDER,
            AUDIO_DEVICE_OUT_EARPIECE, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(INVALID_OPERATION, ret);

    // Registration should fail due to output not found.
    clearPolicyMix();
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_RENDER,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(INVALID_OPERATION, ret);

    // The first time to register valid policy mixes should succeed.
    clearPolicyMix();
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_RENDER,
            AUDIO_DEVICE_OUT_SPEAKER, "", audioConfig, std::vector<PolicyMixTuple>());
    ASSERT_EQ(NO_ERROR, ret);
    // Registering the same policy mixes should fail.
    ret = mManager->registerPolicyMixes(mAudioMixes);
    ASSERT_EQ(INVALID_OPERATION, ret);
}

TEST_F(AudioPolicyManagerTestDynamicPolicy, UnregisterPolicyMixes) {
    status_t ret;
    audio_config_t audioConfig = AUDIO_CONFIG_INITIALIZER;

    audioConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    audioConfig.format = AUDIO_FORMAT_PCM_16_BIT;
    audioConfig.sample_rate = 48000;
    ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, mMixAddress, audioConfig,
            std::vector<PolicyMixTuple>());
    ASSERT_EQ(NO_ERROR, ret);

    // After successfully registering policy mixes, it should be able to unregister.
    ret = mManager->unregisterPolicyMixes(mAudioMixes);
    ASSERT_EQ(NO_ERROR, ret);

    // After unregistering policy mixes successfully, it should fail unregistering
    // the same policy mixes as they are not registered.
    ret = mManager->unregisterPolicyMixes(mAudioMixes);
    ASSERT_EQ(INVALID_OPERATION, ret);

    resetManager(mPrimaryOnlyConfig);
    // Create a fake policy mixes, the unregistration should fail due to no remote
    // submix module found.
    mAudioMixes.add(AudioMix(Vector<AudioMixMatchCriterion>(), MIX_TYPE_PLAYERS,
            audioConfig, MIX_ROUTE_FLAG_LOOP_BACK, String8(mMixAddress.c_str()), 0));
    ret = mManager->unregisterPolicyMixes(mAudioMixes);
    ASSERT_EQ(INVALID_OPERATION, ret);
}

class AudioPolicyManagerTestDPPlaybackReRouting : public AudioPolicyManagerTestDynamicPolicy,
        public testing::WithParamInterface<audio_attributes_t> {
protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<RecordingActivityTracker> mTracker;

    std::vector<PolicyMixTuple> mUsageRules = {
            {AUDIO_USAGE_MEDIA, AUDIO_SOURCE_DEFAULT, RULE_MATCH_ATTRIBUTE_USAGE},
            {AUDIO_USAGE_ALARM, AUDIO_SOURCE_DEFAULT, RULE_MATCH_ATTRIBUTE_USAGE}
    };

    struct audio_port mInjectionPort;
    audio_port_handle_t mPortId = AUDIO_PORT_HANDLE_NONE;
};

void AudioPolicyManagerTestDPPlaybackReRouting::SetUp() {
    AudioPolicyManagerTestDynamicPolicy::SetUp();

    mTracker.reset(new RecordingActivityTracker());

    audio_config_t audioConfig = AUDIO_CONFIG_INITIALIZER;
    audioConfig.channel_mask = AUDIO_CHANNEL_OUT_STEREO;
    audioConfig.format = AUDIO_FORMAT_PCM_16_BIT;
    audioConfig.sample_rate = 48000;
    status_t ret = addPolicyMix(MIX_TYPE_PLAYERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_OUT_REMOTE_SUBMIX, mMixAddress, audioConfig, mUsageRules);
    ASSERT_EQ(NO_ERROR, ret);

    struct audio_port extractionPort;
    findDevicePort(AUDIO_PORT_ROLE_SOURCE, AUDIO_DEVICE_IN_REMOTE_SUBMIX,
            mMixAddress, extractionPort);

    audio_port_handle_t selectedDeviceId = AUDIO_PORT_HANDLE_NONE;
    audio_source_t source = AUDIO_SOURCE_REMOTE_SUBMIX;
    audio_attributes_t attr = {AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN, source, 0, ""};
    std::string tags = "addr=" + mMixAddress;
    strncpy(attr.tags, tags.c_str(), AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1);
    getInputForAttr(attr, mTracker->getRiid(), &selectedDeviceId, AUDIO_FORMAT_PCM_16_BIT,
            AUDIO_CHANNEL_IN_STEREO, 48000 /*sampleRate*/, AUDIO_INPUT_FLAG_NONE, &mPortId);
    ASSERT_EQ(NO_ERROR, mManager->startInput(mPortId));
    ASSERT_EQ(extractionPort.id, selectedDeviceId);

    findDevicePort(AUDIO_PORT_ROLE_SINK, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
            mMixAddress, mInjectionPort);
}

void AudioPolicyManagerTestDPPlaybackReRouting::TearDown() {
    mManager->stopInput(mPortId);
    AudioPolicyManagerTestDynamicPolicy::TearDown();
}

TEST_F(AudioPolicyManagerTestDPPlaybackReRouting, InitSuccess) {
    // SetUp must finish with no assertions
}

TEST_F(AudioPolicyManagerTestDPPlaybackReRouting, Dump) {
    dumpToLog();
}

TEST_P(AudioPolicyManagerTestDPPlaybackReRouting, PlaybackReRouting) {
    const audio_attributes_t attr = GetParam();
    const audio_usage_t usage = attr.usage;

    audio_port_handle_t playbackRoutedPortId;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    getOutputForAttr(&playbackRoutedPortId, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO,
            48000 /*sampleRate*/, AUDIO_OUTPUT_FLAG_NONE, &portId, attr);
    if (std::find_if(begin(mUsageRules), end(mUsageRules), [&usage](const auto &usageRule) {
            return (std::get<0>(usageRule) == usage) &&
            (std::get<2>(usageRule) == RULE_MATCH_ATTRIBUTE_USAGE);}) != end(mUsageRules) ||
            (strncmp(attr.tags, "addr=", strlen("addr=")) == 0 &&
                    strncmp(attr.tags + strlen("addr="), mMixAddress.c_str(),
                    AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - strlen("addr=") - 1) == 0)) {
        EXPECT_EQ(mInjectionPort.id, playbackRoutedPortId);
    } else {
        EXPECT_NE(mInjectionPort.id, playbackRoutedPortId);
    }
}

INSTANTIATE_TEST_CASE_P(
        PlaybackReroutingUsageMatch,
        AudioPolicyManagerTestDPPlaybackReRouting,
        testing::Values(
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_MEDIA,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_ALARM,
                                     AUDIO_SOURCE_DEFAULT, 0, ""}
                )
        );

INSTANTIATE_TEST_CASE_P(
        PlaybackReroutingAddressPriorityMatch,
        AudioPolicyManagerTestDPPlaybackReRouting,
        testing::Values(
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_MEDIA,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_VOICE_COMMUNICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_ALARM,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_NOTIFICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_NOTIFICATION_EVENT,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_SONIFICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_GAME,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_VIRTUAL_SOURCE,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_ASSISTANT,
                                     AUDIO_SOURCE_DEFAULT, 0, "addr=remote_submix_media"}
                )
        );

INSTANTIATE_TEST_CASE_P(
        PlaybackReroutingUnHandledUsages,
        AudioPolicyManagerTestDPPlaybackReRouting,
        testing::Values(
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_VOICE_COMMUNICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_VOICE_COMMUNICATION_SIGNALLING,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_NOTIFICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_TELEPHONY_RINGTONE,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_REQUEST,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_INSTANT,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_NOTIFICATION_COMMUNICATION_DELAYED,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_NOTIFICATION_EVENT,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_ACCESSIBILITY,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_NAVIGATION_GUIDANCE,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC,
                                     AUDIO_USAGE_ASSISTANCE_SONIFICATION,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_GAME,
                                     AUDIO_SOURCE_DEFAULT, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_MUSIC, AUDIO_USAGE_ASSISTANT,
                                     AUDIO_SOURCE_DEFAULT, 0, ""}
                )
        );

class AudioPolicyManagerTestDPMixRecordInjection : public AudioPolicyManagerTestDynamicPolicy,
        public testing::WithParamInterface<audio_attributes_t> {
protected:
    void SetUp() override;
    void TearDown() override;

    std::unique_ptr<RecordingActivityTracker> mTracker;

    std::vector<PolicyMixTuple> mSourceRules = {
        {AUDIO_USAGE_UNKNOWN, AUDIO_SOURCE_CAMCORDER, RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET},
        {AUDIO_USAGE_UNKNOWN, AUDIO_SOURCE_MIC, RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET},
        {AUDIO_USAGE_UNKNOWN, AUDIO_SOURCE_VOICE_COMMUNICATION, RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET}
    };

    struct audio_port mExtractionPort;
    audio_port_handle_t mPortId = AUDIO_PORT_HANDLE_NONE;
};

void AudioPolicyManagerTestDPMixRecordInjection::SetUp() {
    AudioPolicyManagerTestDynamicPolicy::SetUp();

    mTracker.reset(new RecordingActivityTracker());

    audio_config_t audioConfig = AUDIO_CONFIG_INITIALIZER;
    audioConfig.channel_mask = AUDIO_CHANNEL_IN_STEREO;
    audioConfig.format = AUDIO_FORMAT_PCM_16_BIT;
    audioConfig.sample_rate = 48000;
    status_t ret = addPolicyMix(MIX_TYPE_RECORDERS, MIX_ROUTE_FLAG_LOOP_BACK,
            AUDIO_DEVICE_IN_REMOTE_SUBMIX, mMixAddress, audioConfig, mSourceRules);
    ASSERT_EQ(NO_ERROR, ret);

    struct audio_port injectionPort;
    findDevicePort(AUDIO_PORT_ROLE_SINK, AUDIO_DEVICE_OUT_REMOTE_SUBMIX,
            mMixAddress, injectionPort);

    audio_port_handle_t selectedDeviceId = AUDIO_PORT_HANDLE_NONE;
    audio_usage_t usage = AUDIO_USAGE_VIRTUAL_SOURCE;
    audio_attributes_t attr = {AUDIO_CONTENT_TYPE_UNKNOWN, usage, AUDIO_SOURCE_DEFAULT, 0, ""};
    std::string tags = std::string("addr=") + mMixAddress;
    strncpy(attr.tags, tags.c_str(), AUDIO_ATTRIBUTES_TAGS_MAX_SIZE - 1);
    getOutputForAttr(&selectedDeviceId, AUDIO_FORMAT_PCM_16_BIT, AUDIO_CHANNEL_OUT_STEREO,
            48000 /*sampleRate*/, AUDIO_OUTPUT_FLAG_NONE, &mPortId, attr);
    ASSERT_EQ(NO_ERROR, mManager->startOutput(mPortId));
    ASSERT_EQ(injectionPort.id, getDeviceIdFromPatch(mClient->getLastAddedPatch()));

    findDevicePort(AUDIO_PORT_ROLE_SOURCE, AUDIO_DEVICE_IN_REMOTE_SUBMIX,
            mMixAddress, mExtractionPort);
}

void AudioPolicyManagerTestDPMixRecordInjection::TearDown() {
    mManager->stopOutput(mPortId);
    AudioPolicyManagerTestDynamicPolicy::TearDown();
}

TEST_F(AudioPolicyManagerTestDPMixRecordInjection, InitSuccess) {
    // SetUp mush finish with no assertions.
}

TEST_F(AudioPolicyManagerTestDPMixRecordInjection, Dump) {
    dumpToLog();
}

TEST_P(AudioPolicyManagerTestDPMixRecordInjection, RecordingInjection) {
    const audio_attributes_t attr = GetParam();
    const audio_source_t source = attr.source;

    audio_port_handle_t captureRoutedPortId;
    audio_port_handle_t portId = AUDIO_PORT_HANDLE_NONE;
    getInputForAttr(attr, mTracker->getRiid(), &captureRoutedPortId, AUDIO_FORMAT_PCM_16_BIT,
            AUDIO_CHANNEL_IN_STEREO, 48000 /*sampleRate*/, AUDIO_INPUT_FLAG_NONE, &portId);
    if (std::find_if(begin(mSourceRules), end(mSourceRules), [&source](const auto &sourceRule) {
            return (std::get<1>(sourceRule) == source) &&
            (std::get<2>(sourceRule) == RULE_MATCH_ATTRIBUTE_CAPTURE_PRESET);})
            != end(mSourceRules)) {
        EXPECT_EQ(mExtractionPort.id, captureRoutedPortId);
    } else {
        EXPECT_NE(mExtractionPort.id, captureRoutedPortId);
    }
}

// No address priority rule for remote recording, address is a "don't care"
INSTANTIATE_TEST_CASE_P(
        RecordInjectionSourceMatch,
        AudioPolicyManagerTestDPMixRecordInjection,
        testing::Values(
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_CAMCORDER, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_CAMCORDER, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_MIC, 0, "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_MIC, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_VOICE_COMMUNICATION, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_VOICE_COMMUNICATION, 0,
                                     "addr=remote_submix_media"}
                )
        );

// No address priority rule for remote recording
INSTANTIATE_TEST_CASE_P(
        RecordInjectionSourceNotMatch,
        AudioPolicyManagerTestDPMixRecordInjection,
        testing::Values(
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_VOICE_RECOGNITION, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_HOTWORD, 0, ""},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_VOICE_RECOGNITION, 0,
                                     "addr=remote_submix_media"},
                (audio_attributes_t){AUDIO_CONTENT_TYPE_UNKNOWN, AUDIO_USAGE_UNKNOWN,
                                     AUDIO_SOURCE_HOTWORD, 0, "addr=remote_submix_media"}
                )
        );
