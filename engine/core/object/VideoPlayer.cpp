// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "VideoPlayer.h"

#include "Log.h"
#include "media/MediaCache.h"
#include "media/VideoFormats.h"
#include "media/VideoSourceFactory.h"
#include "media/VideoTextureConverter.h"
#include "subsystem/VideoPlayerSystem.h"

#include <algorithm>
#include <cmath>

using namespace jasperengine;

VideoPlayer::VideoPlayer(Scene* scene): EntityHandle(scene){
    addComponent<VideoPlayerComponent>();
}

VideoPlayer::VideoPlayer(Scene* scene, Entity entity): EntityHandle(scene, entity){
}

VideoPlayer::~VideoPlayer(){
}

VideoPlayer::VideoPlayer(const VideoPlayer& rhs) = default;
VideoPlayer& VideoPlayer::operator=(const VideoPlayer& rhs) = default;
VideoPlayer::VideoPlayer(VideoPlayer&& rhs) noexcept = default;
VideoPlayer& VideoPlayer::operator=(VideoPlayer&& rhs) noexcept = default;

VideoPlayerComponent& VideoPlayer::getComponent() const{
    return EntityHandle::getComponent<VideoPlayerComponent>();
}

void VideoPlayer::setEnabled(bool value){
    getComponent<VideoPlayerComponent>().enabled = value;
}

bool VideoPlayer::isEnabled() const{
    return getComponent<VideoPlayerComponent>().enabled;
}

void VideoPlayer::setSource(VideoPlayerSource value){
    getComponent<VideoPlayerComponent>().source = value;
}

VideoPlayerSource VideoPlayer::getSource() const{
    return getComponent<VideoPlayerComponent>().source;
}

void VideoPlayer::setClip(const std::string& value){
    getComponent<VideoPlayerComponent>().clip = value;
}

std::string VideoPlayer::getClip() const{
    return getComponent<VideoPlayerComponent>().clip;
}

void VideoPlayer::setUrl(const std::string& value){
    getComponent<VideoPlayerComponent>().url = value;
}

std::string VideoPlayer::getUrl() const{
    return getComponent<VideoPlayerComponent>().url;
}

void VideoPlayer::setPlayOnAwake(bool value){
    getComponent<VideoPlayerComponent>().playOnAwake = value;
}

bool VideoPlayer::isPlayOnAwake() const{
    return getComponent<VideoPlayerComponent>().playOnAwake;
}

void VideoPlayer::setWaitForFirstFrame(bool value){
    getComponent<VideoPlayerComponent>().waitForFirstFrame = value;
}

bool VideoPlayer::isWaitForFirstFrame() const{
    return getComponent<VideoPlayerComponent>().waitForFirstFrame;
}

void VideoPlayer::setLooping(bool value){
    getComponent<VideoPlayerComponent>().isLooping = value;
}

bool VideoPlayer::isLooping() const{
    return getComponent<VideoPlayerComponent>().isLooping;
}

void VideoPlayer::setSkipOnDrop(bool value){
    getComponent<VideoPlayerComponent>().skipOnDrop = value;
}

bool VideoPlayer::isSkipOnDrop() const{
    return getComponent<VideoPlayerComponent>().skipOnDrop;
}

void VideoPlayer::setPlaybackSpeed(float value){
    if (!std::isfinite(value)){
        Log::error("VideoPlayer.playbackSpeed: %g is not a finite number", value);
        return;
    }
    getComponent<VideoPlayerComponent>().playbackSpeed = std::clamp(value, 0.0625f, 16.0f);
}

float VideoPlayer::getPlaybackSpeed() const{
    return getComponent<VideoPlayerComponent>().playbackSpeed;
}

void VideoPlayer::setTimeUpdateMode(VideoTimeUpdateMode value){
    getComponent<VideoPlayerComponent>().timeUpdateMode = value;
}

VideoTimeUpdateMode VideoPlayer::getTimeUpdateMode() const{
    return getComponent<VideoPlayerComponent>().timeUpdateMode;
}

void VideoPlayer::setRenderMode(VideoRenderMode value){
    getComponent<VideoPlayerComponent>().renderMode = value;
}

VideoRenderMode VideoPlayer::getRenderMode() const{
    return getComponent<VideoPlayerComponent>().renderMode;
}

void VideoPlayer::setTargetCamera(Entity value){
    getComponent<VideoPlayerComponent>().targetCamera = value;
}

Entity VideoPlayer::getTargetCamera() const{
    return getComponent<VideoPlayerComponent>().targetCamera;
}

void VideoPlayer::setTargetCameraAlpha(float value){
    if (!std::isfinite(value)){
        Log::error("VideoPlayer.targetCameraAlpha: %g is not a finite number", value);
        return;
    }
    getComponent<VideoPlayerComponent>().targetCameraAlpha = std::clamp(value, 0.0f, 1.0f);
}

float VideoPlayer::getTargetCameraAlpha() const{
    return getComponent<VideoPlayerComponent>().targetCameraAlpha;
}

void VideoPlayer::setAspectRatio(VideoAspectRatio value){
    getComponent<VideoPlayerComponent>().aspectRatio = value;
}

VideoAspectRatio VideoPlayer::getAspectRatio() const{
    return getComponent<VideoPlayerComponent>().aspectRatio;
}

void VideoPlayer::setTargetMaterialRenderer(Entity value){
    getComponent<VideoPlayerComponent>().targetMaterialRenderer = value;
}

Entity VideoPlayer::getTargetMaterialRenderer() const{
    return getComponent<VideoPlayerComponent>().targetMaterialRenderer;
}

void VideoPlayer::setTargetMaterialProperty(const std::string& value){
    getComponent<VideoPlayerComponent>().targetMaterialProperty = value;
}

std::string VideoPlayer::getTargetMaterialProperty() const{
    return getComponent<VideoPlayerComponent>().targetMaterialProperty;
}

void VideoPlayer::setTargetSubmesh(int value){
    getComponent<VideoPlayerComponent>().targetSubmesh = value;
}

int VideoPlayer::getTargetSubmesh() const{
    return getComponent<VideoPlayerComponent>().targetSubmesh;
}

void VideoPlayer::setRenderTextureWidth(unsigned int value){
    getComponent<VideoPlayerComponent>().renderTextureWidth = value;
}

unsigned int VideoPlayer::getRenderTextureWidth() const{
    return getComponent<VideoPlayerComponent>().renderTextureWidth;
}

void VideoPlayer::setRenderTextureHeight(unsigned int value){
    getComponent<VideoPlayerComponent>().renderTextureHeight = value;
}

unsigned int VideoPlayer::getRenderTextureHeight() const{
    return getComponent<VideoPlayerComponent>().renderTextureHeight;
}

void VideoPlayer::setAudioOutputMode(VideoAudioOutputMode value){
    getComponent<VideoPlayerComponent>().audioOutputMode = value;
}

VideoAudioOutputMode VideoPlayer::getAudioOutputMode() const{
    return getComponent<VideoPlayerComponent>().audioOutputMode;
}

void VideoPlayer::setDirectVolume(float value){
    if (!std::isfinite(value)){
        Log::error("VideoPlayer.directVolume: %g is not a finite number", value);
        return;
    }
    getComponent<VideoPlayerComponent>().directVolume = std::clamp(value, 0.0f, 1.0f);
}

float VideoPlayer::getDirectVolume() const{
    return getComponent<VideoPlayerComponent>().directVolume;
}

void VideoPlayer::setDirectMute(bool value){
    getComponent<VideoPlayerComponent>().directMute = value;
}

bool VideoPlayer::isDirectMute() const{
    return getComponent<VideoPlayerComponent>().directMute;
}

void VideoPlayer::setTargetAudioSource(Entity value){
    getComponent<VideoPlayerComponent>().targetAudioSource = value;
}

Entity VideoPlayer::getTargetAudioSource() const{
    return getComponent<VideoPlayerComponent>().targetAudioSource;
}

namespace {

    VideoPlayerSystem* systemOf(Scene* scene){
        return scene ? scene->getSystem<VideoPlayerSystem>().get() : nullptr;
    }

}

void VideoPlayer::prepare(){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->prepare(entity);
}

void VideoPlayer::play(){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->play(entity);
}

void VideoPlayer::pause(){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->pause(entity);
}

void VideoPlayer::stop(){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->stop(entity);
}

void VideoPlayer::stepForward(){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->stepForward(entity);
}

void VideoPlayer::setTime(double seconds){
    if (!std::isfinite(seconds)){
        Log::error("VideoPlayer.time: %g is not a finite number", seconds);
        return;
    }
    if (VideoPlayerSystem* system = systemOf(scene))
        system->setTime(entity, std::max(seconds, 0.0));
}

double VideoPlayer::getTime() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).time : 0.0;
}

bool VideoPlayer::canSetTime() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system && system->getStatus(entity).canSetTime;
}

void VideoPlayer::setFrame(int64_t frame){
    if (VideoPlayerSystem* system = systemOf(scene))
        system->setFrame(entity, frame);
}

int64_t VideoPlayer::getFrame() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).frame : -1;
}

int64_t VideoPlayer::getFrameCount() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).frameCount : 0;
}

double VideoPlayer::getFrameRate() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).frameRate : 0.0;
}

double VideoPlayer::getLength() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).length : 0.0;
}

int VideoPlayer::getWidth() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).width : 0;
}

int VideoPlayer::getHeight() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).height : 0;
}

bool VideoPlayer::isPrepared() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system && system->getStatus(entity).prepared;
}

bool VideoPlayer::isPlaying() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system && system->getStatus(entity).playing;
}

bool VideoPlayer::isPaused() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system && system->getStatus(entity).paused;
}

double VideoPlayer::getPrepareProgress() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).prepareProgress : 0.0;
}

std::string VideoPlayer::getBackend() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).backend : std::string();
}

std::string VideoPlayer::getVideoCodec() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).videoCodec : std::string();
}

std::string VideoPlayer::getAudioCodec() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).audioCodec : std::string();
}

std::string VideoPlayer::getError() const{
    VideoPlayerSystem* system = systemOf(scene);
    return system ? system->getStatus(entity).error : std::string();
}

Framebuffer* VideoPlayer::getFramebuffer() const{
    VideoPlayerSystem* system = systemOf(scene);
    VideoTextureConverter* output = system ? system->getOutput(entity) : nullptr;
    return output ? output->getFramebuffer() : nullptr;
}

std::optional<Texture> VideoPlayer::getTexture() const{
    Framebuffer* framebuffer = getFramebuffer();
    if (!framebuffer || !isPrepared())
        return std::nullopt;
    Texture texture;
    texture.setFramebuffer(framebuffer);
    return texture;
}

std::string VideoPlayer::canPlay(const std::string& pathOrUrl){
    if (pathOrUrl.empty())
        return "";
    if (VideoSourceFactory::isUrl(pathOrUrl))
        return VideoFormats::toString(VideoFormats::canPlayExtension(pathOrUrl));
    const VideoProbe probe = VideoFormats::probeFile(pathOrUrl);
    if (!probe.playable)
        return "";
    std::string reason;
    return VideoSourceFactory::probeSupport(probe, &reason) == VideoSourceFactory::Support::YES ? "probably" : "maybe";
}

int VideoPlayer::clearCache(const std::string& url){
    return MediaCache::clear(url);
}

FunctionSubscribe<void(VideoPlayer)>& VideoPlayer::getOnPrepareCompleted(){
    return getComponent().onPrepareCompleted;
}

FunctionSubscribe<void(VideoPlayer)>& VideoPlayer::getOnStarted(){
    return getComponent().onStarted;
}

FunctionSubscribe<void(VideoPlayer)>& VideoPlayer::getOnLoopPointReached(){
    return getComponent().onLoopPointReached;
}

FunctionSubscribe<void(VideoPlayer, const std::string&)>& VideoPlayer::getOnErrorReceived(){
    return getComponent().onErrorReceived;
}

FunctionSubscribe<void(VideoPlayer)>& VideoPlayer::getOnSeekCompleted(){
    return getComponent().onSeekCompleted;
}

FunctionSubscribe<void(VideoPlayer, int)>& VideoPlayer::getOnFrameReady(){
    return getComponent().onFrameReady;
}
