/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <igl/opengl/Framebuffer.h>

#include <cstdlib>
#include <igl/RenderPass.h>
#include <igl/opengl/CommandBuffer.h>
#include <igl/opengl/Device.h>
#include <igl/opengl/DummyTexture.h>
#include <igl/opengl/Errors.h>
#include <igl/opengl/Texture.h>

#include <algorithm>
#if !IGL_PLATFORM_ANDROID
#include <string>
#else
#include <sstream>

namespace std {

// TODO: Remove once STL in Android NDK supports std::to_string
template<typename T>
string to_string(const T& t) {
  ostringstream os;
  os << t;
  return os.str();
}

} // namespace std

#endif

namespace igl::opengl {

namespace {

Result checkFramebufferStatus(IContext& context, bool read) {
  auto code = Result::Code::Ok;
  std::string message;
  GLenum framebufferTarget = GL_FRAMEBUFFER;
  if (context.deviceFeatures().hasFeature(DeviceFeatures::ReadWriteFramebuffer)) {
    framebufferTarget = read ? GL_READ_FRAMEBUFFER : GL_DRAW_FRAMEBUFFER;
  }
  // check that we've created a proper frame buffer
  GLenum status = context.checkFramebufferStatus(framebufferTarget);
  if (status != GL_FRAMEBUFFER_COMPLETE) {
    code = Result::Code::RuntimeError;

    switch (status) {
    case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
      message = "GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT";
      break;
    case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
      message = "GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT";
      break;
    case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS:
      message = "GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS";
      break;
    case GL_FRAMEBUFFER_UNSUPPORTED:
      message = "GL_FRAMEBUFFER_UNSUPPORTED";
      break;
    default:
      message = "GL_FRAMEBUFFER unknown error: " + std::to_string(status);
      break;
    }
  }

  return Result(code, message);
}

void attachAsColor(igl::ITexture& texture,
                   uint32_t index,
                   const Texture::AttachmentParams& params) {
  static_cast<Texture&>(texture).attachAsColor(index, params);
}

void attachAsDepth(igl::ITexture& texture, const Texture::AttachmentParams& params) {
  static_cast<Texture&>(texture).attachAsDepth(params);
}

void attachAsStencil(igl::ITexture& texture, const Texture::AttachmentParams& params) {
  static_cast<Texture&>(texture).attachAsStencil(params);
}

Texture::AttachmentParams toAttachmentParams(const RenderPassDesc::BaseAttachmentDesc& attachment,
                                             FramebufferMode mode) {
  Texture::AttachmentParams params{};
  params.face = attachment.face;
  params.mipLevel = attachment.mipLevel;
  params.layer = attachment.layer;
  params.read = false; // Color attachments are for writing
  params.stereo = mode == FramebufferMode::Stereo;
  return params;
}

Texture::AttachmentParams defaultWriteAttachmentParams(FramebufferMode mode) {
  Texture::AttachmentParams params{};
  params.face = 0;
  params.mipLevel = 0;
  params.layer = 0;
  params.read = false;
  params.stereo = mode == FramebufferMode::Stereo;
  return params;
}

Texture::AttachmentParams toReadAttachmentParams(const TextureRangeDesc& range,
                                                 FramebufferMode mode) {
  IGL_ASSERT_MSG(range.numLayers == 1, "range.numLayers must be 1.");
  IGL_ASSERT_MSG(range.numMipLevels == 1, "range.numMipLevels must be 1.");
  IGL_ASSERT_MSG(range.numFaces == 1, "range.numFaces must be 1.");

  Texture::AttachmentParams params{};
  params.face = static_cast<uint32_t>(range.face);
  params.mipLevel = static_cast<uint32_t>(range.mipLevel);
  params.layer = static_cast<uint32_t>(range.layer);
  params.read = true;
  params.stereo = mode == FramebufferMode::Stereo;
  return params;
}
} // namespace

FramebufferBindingGuard::FramebufferBindingGuard(IContext& context) :
  context_(context),
  currentRenderbuffer_(0),
  currentFramebuffer_(0),
  currentReadFramebuffer_(0),
  currentDrawFramebuffer_(0) {
  context_.getIntegerv(GL_RENDERBUFFER_BINDING, reinterpret_cast<GLint*>(&currentRenderbuffer_));

  // Only restore currently bound framebuffer if it's valid
  if (context.deviceFeatures().hasFeature(DeviceFeatures::ReadWriteFramebuffer)) {
    if (checkFramebufferStatus(context, true).isOk()) {
      context_.getIntegerv(GL_READ_FRAMEBUFFER_BINDING,
                           reinterpret_cast<GLint*>(&currentReadFramebuffer_));
    }
    if (checkFramebufferStatus(context, false).isOk()) {
      context_.getIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,
                           reinterpret_cast<GLint*>(&currentDrawFramebuffer_));
    }
  } else {
    if (checkFramebufferStatus(context, false).isOk()) {
      context_.getIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&currentFramebuffer_));
    }
  }
}

FramebufferBindingGuard::~FramebufferBindingGuard() {
  if (context_.deviceFeatures().hasFeature(DeviceFeatures::ReadWriteFramebuffer)) {
    context_.bindFramebuffer(GL_READ_FRAMEBUFFER, currentReadFramebuffer_);
    context_.bindFramebuffer(GL_DRAW_FRAMEBUFFER, currentDrawFramebuffer_);
  } else {
    context_.bindFramebuffer(GL_FRAMEBUFFER, currentFramebuffer_);
  }

  context_.bindRenderbuffer(GL_RENDERBUFFER, currentRenderbuffer_);
}

///--------------------------------------
/// MARK: - Framebuffer

Framebuffer::Framebuffer(IContext& context) : WithContext(context) {}

void Framebuffer::bindBuffer() const {
  getContext().bindFramebuffer(GL_FRAMEBUFFER, frameBufferID_);
}

void Framebuffer::bindBufferForRead() const {
  // TODO: enable optimization path
  if (getContext().deviceFeatures().hasFeature(DeviceFeatures::ReadWriteFramebuffer)) {
    getContext().bindFramebuffer(GL_READ_FRAMEBUFFER, frameBufferID_);
  } else {
    bindBuffer();
  }
}

void Framebuffer::copyBytesColorAttachment(ICommandQueue& /* unused */,
                                           size_t index,
                                           void* pixelBytes,
                                           const TextureRangeDesc& range,
                                           size_t bytesPerRow) const {
  // Only support attachment 0 because that's what glReadPixels supports
  if (index != 0) {
    IGL_ASSERT_MSG(0, "Invalid index: %d", index);
    return;
  }
  IGL_ASSERT_MSG(range.numFaces == 1, "range.numFaces MUST be 1");
  IGL_ASSERT_MSG(range.numLayers == 1, "range.numLayers MUST be 1");
  IGL_ASSERT_MSG(range.numMipLevels == 1, "range.numMipLevels MUST be 1");

  auto itexture = getColorAttachment(index);
  if (itexture != nullptr) {
    FramebufferBindingGuard guard(getContext());

    CustomFramebuffer extraFramebuffer(getContext());

    auto& texture = static_cast<igl::opengl::Texture&>(*itexture);

    Result ret;
    FramebufferDesc desc;
    desc.colorAttachments[0].texture = itexture;
    extraFramebuffer.initialize(desc, &ret);
    IGL_ASSERT_MSG(ret.isOk(), ret.message.c_str());

    extraFramebuffer.bindBufferForRead();
    attachAsColor(*itexture, 0, toReadAttachmentParams(range, FramebufferMode::Mono));
    checkFramebufferStatus(getContext(), true);

    if (bytesPerRow == 0) {
      bytesPerRow = itexture->getProperties().getBytesPerRow(range);
    }
    getContext().pixelStorei(GL_PACK_ALIGNMENT, texture.getAlignment(bytesPerRow, range.mipLevel));

    // Note read out format is based on
    // (https://www.khronos.org/registry/OpenGL-Refpages/es2.0/xhtml/glReadPixels.xml)
    // as using GL_RGBA with GL_UNSIGNED_BYTE is the only always supported combination
    // with glReadPixels.
    getContext().flush();
    auto format = GL_RGBA;
    auto intFormat = GL_RGBA_INTEGER;

    // @fb-only
    if (texture.getFormat() == TextureFormat::RGBA_UInt32) {
      if (getContext().deviceFeatures().hasTextureFeature(TextureFeatures::TextureInteger)) {
        getContext().readPixels(static_cast<GLint>(range.x),
                                static_cast<GLint>(range.y),
                                static_cast<GLsizei>(range.width),
                                static_cast<GLsizei>(range.height),
                                intFormat,
                                GL_UNSIGNED_INT,
                                pixelBytes);
      } else {
        IGL_ASSERT_NOT_IMPLEMENTED();
      }
    } else {
      getContext().readPixels(static_cast<GLint>(range.x),
                              static_cast<GLint>(range.y),
                              static_cast<GLsizei>(range.width),
                              static_cast<GLsizei>(range.height),
                              format,
                              GL_UNSIGNED_BYTE,
                              pixelBytes);
    }
    getContext().checkForErrors(nullptr, 0);
    auto error = getContext().getLastError();
    IGL_ASSERT_MSG(error.isOk(), error.message.c_str());
  } else {
    IGL_ASSERT_NOT_IMPLEMENTED();
  }
}

void Framebuffer::copyBytesDepthAttachment(ICommandQueue& /* unused */,
                                           void* /*pixelBytes*/,
                                           const TextureRangeDesc& /*range*/,
                                           size_t /*bytesPerRow*/) const {
  IGL_ASSERT_NOT_IMPLEMENTED();
}

void Framebuffer::copyBytesStencilAttachment(ICommandQueue& /* unused */,
                                             void* /*pixelBytes*/,
                                             const TextureRangeDesc& /*range*/,
                                             size_t /*bytesPerRow*/) const {
  IGL_ASSERT_NOT_IMPLEMENTED();
}

void Framebuffer::copyTextureColorAttachment(ICommandQueue& /*cmdQueue*/,
                                             size_t index,
                                             std::shared_ptr<ITexture> destTexture,
                                             const TextureRangeDesc& range) const {
  // Only support attachment 0 because that's what glCopyTexImage2D supports
  if (index != 0 || getColorAttachment(index) == nullptr) {
    IGL_ASSERT_MSG(0, "Invalid index: %d", index);
    return;
  }

  FramebufferBindingGuard guard(getContext());

  bindBufferForRead();

  auto& dest = static_cast<Texture&>(*destTexture);
  dest.bind();

  getContext().copyTexSubImage2D(GL_TEXTURE_2D,
                                 0,
                                 0,
                                 0,
                                 static_cast<GLint>(range.x),
                                 static_cast<GLint>(range.y),
                                 static_cast<GLsizei>(range.width),
                                 static_cast<GLsizei>(range.height));
}

///--------------------------------------
/// MARK: - CustomFramebuffer

CustomFramebuffer::~CustomFramebuffer() {
  if (frameBufferID_ != 0) {
    getContext().deleteFramebuffers(1, &frameBufferID_);
    frameBufferID_ = 0;
  }
}

std::vector<size_t> CustomFramebuffer::getColorAttachmentIndices() const {
  std::vector<size_t> indices;

  for (const auto& attachment : renderTarget_.colorAttachments) {
    indices.push_back(attachment.first);
  }

  return indices;
}

std::shared_ptr<ITexture> CustomFramebuffer::getColorAttachment(size_t index) const {
  auto colorAttachment = renderTarget_.colorAttachments.find(index);

  if (colorAttachment != renderTarget_.colorAttachments.end()) {
    return colorAttachment->second.texture;
  }

  return nullptr;
}

std::shared_ptr<ITexture> CustomFramebuffer::getResolveColorAttachment(size_t index) const {
  auto colorAttachment = renderTarget_.colorAttachments.find(index);

  if (colorAttachment != renderTarget_.colorAttachments.end()) {
    return colorAttachment->second.resolveTexture;
  }

  return nullptr;
}

std::shared_ptr<ITexture> CustomFramebuffer::getDepthAttachment() const {
  return renderTarget_.depthAttachment.texture;
}

std::shared_ptr<ITexture> CustomFramebuffer::getResolveDepthAttachment() const {
  return renderTarget_.depthAttachment.resolveTexture;
}

std::shared_ptr<ITexture> CustomFramebuffer::getStencilAttachment() const {
  return renderTarget_.stencilAttachment.texture;
}

FramebufferMode CustomFramebuffer::getMode() const {
  return renderTarget_.mode;
}

void CustomFramebuffer::updateDrawable(std::shared_ptr<ITexture> texture) {
  updateDrawableInternal({std::move(texture), nullptr}, false);
}

void CustomFramebuffer::updateDrawable(SurfaceTextures surfaceTextures) {
  updateDrawableInternal(std::move(surfaceTextures), true);
}

void CustomFramebuffer::updateDrawableInternal(SurfaceTextures surfaceTextures, bool updateDepth) {
  auto colorAttachment0 = getColorAttachment(0);
  auto depthAttachment = updateDepth ? getDepthAttachment() : nullptr;

  const bool updateColor = colorAttachment0 != surfaceTextures.color;
  updateDepth = updateDepth && depthAttachment != surfaceTextures.depth;
  if (updateColor || updateDepth) {
    FramebufferBindingGuard guard(getContext());
    bindBuffer();
    if (updateColor) {
      if (!surfaceTextures.color) {
        static_cast<Texture&>(*colorAttachment0).detachAsColor(0, false);
        renderTarget_.colorAttachments.erase(0);
      } else {
        attachAsColor(*surfaceTextures.color, 0, defaultWriteAttachmentParams(renderTarget_.mode));

        renderTarget_.colorAttachments[0].texture = std::move(surfaceTextures.color);
      }
    }
    if (updateDepth) {
      if (!surfaceTextures.depth) {
        static_cast<Texture&>(*colorAttachment0).detachAsDepth(false);
        renderTarget_.depthAttachment.texture = nullptr;
      } else {
        attachAsDepth(*surfaceTextures.depth, defaultWriteAttachmentParams(renderTarget_.mode));

        renderTarget_.depthAttachment.texture = std::move(surfaceTextures.depth);
      }
    }
  }
}

bool CustomFramebuffer::isInitialized() const {
  return initialized_;
}

bool CustomFramebuffer::hasImplicitColorAttachment() const {
  if (frameBufferID_ != 0) {
    return false;
  }

  auto colorAttachment0 = renderTarget_.colorAttachments.find(0);

  return colorAttachment0 != renderTarget_.colorAttachments.end() &&
         colorAttachment0->second.texture != nullptr &&
         static_cast<Texture&>(*colorAttachment0->second.texture).isImplicitStorage();
}

void CustomFramebuffer::initialize(const FramebufferDesc& desc, Result* outResult) {
  if (IGL_UNEXPECTED(isInitialized())) {
    Result::setResult(outResult, Result::Code::RuntimeError, "Framebuffer already initialized.");
    return;
  }
  initialized_ = true;

  renderTarget_ = desc;

  // Restore framebuffer binding
  FramebufferBindingGuard guard(getContext());
  if (!desc.debugName.empty() &&
      getContext().deviceFeatures().hasInternalFeature(InternalFeatures::DebugLabel)) {
    getContext().objectLabel(
        GL_FRAMEBUFFER, frameBufferID_, desc.debugName.size(), desc.debugName.c_str());
  }
  if (hasImplicitColorAttachment()) {
    // Don't generate framebuffer id. Use implicit framebuffer supplied by containing view
    Result::setOk(outResult);
  } else {
    prepareResource(outResult);
  }
}

void CustomFramebuffer::prepareResource(Result* outResult) {
  // create a new frame buffer if we don't already have one
  getContext().genFramebuffers(1, &frameBufferID_);

  bindBuffer();

  std::vector<GLenum> drawBuffers;

  const auto attachmentParams = defaultWriteAttachmentParams(renderTarget_.mode);
  // attach the textures and render buffers to the frame buffer
  for (const auto& colorAttachment : renderTarget_.colorAttachments) {
    auto const colorAttachmentTexture = colorAttachment.second.texture;
    if (colorAttachmentTexture != nullptr) {
      size_t index = colorAttachment.first;
      attachAsColor(*colorAttachmentTexture, static_cast<uint32_t>(index), attachmentParams);
      drawBuffers.push_back(static_cast<GLenum>(GL_COLOR_ATTACHMENT0 + index));
    }
  }

  std::sort(drawBuffers.begin(), drawBuffers.end());

  if (drawBuffers.size() > 1) {
    getContext().drawBuffers(static_cast<GLsizei>(drawBuffers.size()), drawBuffers.data());
  }

  if (renderTarget_.depthAttachment.texture != nullptr) {
    attachAsDepth(*renderTarget_.depthAttachment.texture, attachmentParams);
  }

  if (renderTarget_.stencilAttachment.texture != nullptr) {
    attachAsStencil(*renderTarget_.stencilAttachment.texture, attachmentParams);
  }

  Result result = checkFramebufferStatus(getContext(), false);
  IGL_ASSERT_MSG(result.isOk(), result.message.c_str());
  if (outResult) {
    *outResult = result;
  }
  if (!result.isOk()) {
    return;
  }

  // Check if resolve framebuffer is needed
  FramebufferDesc resolveDesc;
  auto createResolveFramebuffer = false;
  for (const auto& colorAttachment : renderTarget_.colorAttachments) {
    if (colorAttachment.second.resolveTexture) {
      createResolveFramebuffer = true;
      FramebufferDesc::AttachmentDesc attachment;
      attachment.texture = colorAttachment.second.resolveTexture;
      resolveDesc.colorAttachments.emplace(colorAttachment.first, attachment);
    }
  }
  if (createResolveFramebuffer &&
      resolveDesc.colorAttachments.size() != renderTarget_.colorAttachments.size()) {
    IGL_ASSERT_NOT_REACHED();
    if (outResult) {
      *outResult = igl::Result(igl::Result::Code::ArgumentInvalid,
                               "If resolve texture is specified on a color attachment it must be "
                               "specified on all of them");
    }
    return;
  }

  if (renderTarget_.depthAttachment.resolveTexture) {
    createResolveFramebuffer = true;
    resolveDesc.depthAttachment.texture = renderTarget_.depthAttachment.resolveTexture;
  }
  if (renderTarget_.stencilAttachment.resolveTexture) {
    createResolveFramebuffer = true;
    resolveDesc.stencilAttachment.texture = renderTarget_.stencilAttachment.resolveTexture;
  }

  if (createResolveFramebuffer) {
    auto cfb = std::make_shared<CustomFramebuffer>(getContext());
    cfb->initialize(resolveDesc, &result);
    if (outResult) {
      *outResult = result;
    }
    resolveFramebuffer = std::move(cfb);
  }
}

Viewport CustomFramebuffer::getViewport() const {
  auto texture = getColorAttachment(0);

  if (texture == nullptr) {
    texture = getDepthAttachment();
  }

  if (texture == nullptr) {
    IGL_ASSERT_MSG(0, "No color/depth attachments in CustomFrameBuffer at index 0");
    return {0, 0, 0, 0};
  }

  // By default, we set viewport to dimensions of framebuffer
  const auto size = texture->getSize();
  return {0, 0, size.width, size.height};
}

void CustomFramebuffer::bind(const RenderPassDesc& renderPass) const {
  // Cache renderPass for unbind
  renderPass_ = renderPass;
  IGL_ASSERT_MSG(renderTarget_.mode != FramebufferMode::Multiview,
                 "FramebufferMode::Multiview not supported");

  bindBuffer();

  for (auto colorAttachment : renderTarget_.colorAttachments) {
    auto const colorAttachmentTexture = colorAttachment.second.texture;

    if (colorAttachmentTexture == nullptr) {
      continue;
    }
#if !IGL_OPENGL_ES
    // OpenGL ES doesn't need to call glEnable. All it needs is an sRGB framebuffer.
    if (getContext().deviceFeatures().hasFeature(DeviceFeatures::SRGB)) {
      if (colorAttachmentTexture->getProperties().isSRGB()) {
        getContext().enable(GL_FRAMEBUFFER_SRGB);
      } else {
        getContext().disable(GL_FRAMEBUFFER_SRGB);
      }
    }
#endif
    const size_t index = colorAttachment.first;
    IGL_ASSERT(index >= 0 && index < renderPass.colorAttachments.size());
    const auto& renderPassAttachment = renderPass.colorAttachments[index];
    // When setting up a framebuffer, we attach textures as though they were a non-array
    // texture with and set layer, mip level and face equal to 0.
    // If any of these assumptions are not true, we need to reattach with proper values.
    const bool needsToBeReattached =
        renderTarget_.mode == FramebufferMode::Stereo || renderPassAttachment.layer > 0 ||
        renderPassAttachment.face > 0 || renderPassAttachment.mipLevel > 0;

    if (needsToBeReattached) {
      attachAsColor(*colorAttachmentTexture,
                    static_cast<uint32_t>(index),
                    toAttachmentParams(renderPassAttachment, renderTarget_.mode));
    }
  }
  if (renderTarget_.depthAttachment.texture) {
    const auto& renderPassAttachment = renderPass.depthAttachment;
    const bool needsToBeReattached =
        renderTarget_.mode == FramebufferMode::Stereo || renderPassAttachment.layer > 0 ||
        renderPassAttachment.face > 0 || renderPassAttachment.mipLevel > 0;
    if (needsToBeReattached) {
      attachAsDepth(*renderTarget_.depthAttachment.texture,
                    toAttachmentParams(renderPassAttachment, renderTarget_.mode));
    }
  }
  // clear the buffers if we're not loading previous contents
  GLbitfield clearMask = 0;
  auto colorAttachment0 = renderTarget_.colorAttachments.find(0);

  if (colorAttachment0 != renderTarget_.colorAttachments.end() &&
      colorAttachment0->second.texture != nullptr &&
      renderPass_.colorAttachments[0].loadAction == LoadAction::Clear) {
    clearMask |= GL_COLOR_BUFFER_BIT;
    auto clearColor = renderPass_.colorAttachments[0].clearColor;
    getContext().colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    getContext().clearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
  }
  if (renderTarget_.depthAttachment.texture != nullptr) {
    if (renderPass_.depthAttachment.loadAction == LoadAction::Clear) {
      clearMask |= GL_DEPTH_BUFFER_BIT;
      getContext().depthMask(GL_TRUE);
      getContext().clearDepthf(renderPass_.depthAttachment.clearDepth);
    }
  }
  if (renderTarget_.stencilAttachment.texture != nullptr) {
    getContext().enable(GL_STENCIL_TEST);
    if (renderPass_.stencilAttachment.loadAction == LoadAction::Clear) {
      clearMask |= GL_STENCIL_BUFFER_BIT;
      getContext().stencilMask(0xFF);
      getContext().clearStencil(renderPass_.stencilAttachment.clearStencil);
    }
  }

  if (clearMask != 0) {
    getContext().clear(clearMask);
  }
}

void CustomFramebuffer::unbind() const {
  // discard the depthStencil if we don't need to store its contents
  GLenum attachments[3];
  GLsizei numAttachments = 0;
  auto colorAttachment0 = renderTarget_.colorAttachments.find(0);

  if (colorAttachment0 != renderTarget_.colorAttachments.end() &&
      colorAttachment0->second.texture != nullptr &&
      renderPass_.colorAttachments[0].storeAction != StoreAction::Store) {
    attachments[numAttachments++] = GL_COLOR_ATTACHMENT0;
  }
  if (renderTarget_.depthAttachment.texture != nullptr) {
    if (renderPass_.depthAttachment.storeAction != StoreAction::Store) {
      attachments[numAttachments++] = GL_DEPTH_ATTACHMENT;
    }
  }
  if (renderTarget_.stencilAttachment.texture != nullptr) {
    getContext().disable(GL_STENCIL_TEST);
    if (renderPass_.stencilAttachment.storeAction != StoreAction::Store) {
      attachments[numAttachments++] = GL_STENCIL_ATTACHMENT;
    }
  }

  if (numAttachments > 0) {
    auto& features = getContext().deviceFeatures();
    if (features.hasInternalFeature(InternalFeatures::InvalidateFramebuffer)) {
      getContext().invalidateFramebuffer(GL_FRAMEBUFFER, numAttachments, attachments);
    }
  }
}

///--------------------------------------
/// MARK: - CurrentFramebuffer

CurrentFramebuffer::CurrentFramebuffer(IContext& context) : Super(context) {
  getContext().getIntegerv(GL_FRAMEBUFFER_BINDING, reinterpret_cast<GLint*>(&frameBufferID_));

  GLint viewport[4];
  getContext().getIntegerv(GL_VIEWPORT, viewport);
  viewport_.x = static_cast<float>(viewport[0]);
  viewport_.y = static_cast<float>(viewport[1]);
  viewport_.width = static_cast<float>(viewport[2]);
  viewport_.height = static_cast<float>(viewport[3]);

  colorAttachment_ = std::make_shared<DummyTexture>(Size(viewport_.width, viewport_.height));
}

std::vector<size_t> CurrentFramebuffer::getColorAttachmentIndices() const {
  return std::vector<size_t>{0};
}

std::shared_ptr<ITexture> CurrentFramebuffer::getColorAttachment(size_t index) const {
  if (index != 0) {
    IGL_ASSERT_NOT_REACHED();
  }
  return colorAttachment_;
}

std::shared_ptr<ITexture> CurrentFramebuffer::getResolveColorAttachment(size_t index) const {
  if (index != 0) {
    IGL_ASSERT_NOT_REACHED();
  }
  return colorAttachment_;
}

std::shared_ptr<ITexture> CurrentFramebuffer::getDepthAttachment() const {
  return nullptr;
}

std::shared_ptr<ITexture> CurrentFramebuffer::getResolveDepthAttachment() const {
  return nullptr;
}

std::shared_ptr<ITexture> CurrentFramebuffer::getStencilAttachment() const {
  return nullptr;
}

void CurrentFramebuffer::updateDrawable(std::shared_ptr<ITexture> /*texture*/) {
  IGL_ASSERT_NOT_REACHED();
}

void CurrentFramebuffer::updateDrawable(SurfaceTextures /*surfaceTextures*/) {
  IGL_ASSERT_NOT_REACHED();
}

Viewport CurrentFramebuffer::getViewport() const {
  return viewport_;
}

void CurrentFramebuffer::bind(const RenderPassDesc& renderPass) const {
  bindBuffer();
#if !IGL_OPENGL_ES
  // OpenGL ES doesn't need to call glEnable. All it needs is an sRGB framebuffer.
  auto colorAttach = getResolveColorAttachment(getColorAttachmentIndices()[0]);
  if (getContext().deviceFeatures().hasFeature(DeviceFeatures::SRGB)) {
    if (colorAttach && colorAttach->getProperties().isSRGB()) {
      getContext().enable(GL_FRAMEBUFFER_SRGB);
    } else {
      getContext().disable(GL_FRAMEBUFFER_SRGB);
    }
  }
#endif

  // clear the buffers if we're not loading previous contents
  GLbitfield clearMask = 0;
  if (renderPass.colorAttachments[0].loadAction != LoadAction::Load) {
    clearMask |= GL_COLOR_BUFFER_BIT;
    auto clearColor = renderPass.colorAttachments[0].clearColor;
    getContext().colorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    getContext().clearColor(clearColor.r, clearColor.g, clearColor.b, clearColor.a);
  }
  if (renderPass.depthAttachment.loadAction != LoadAction::Load) {
    clearMask |= GL_DEPTH_BUFFER_BIT;
    getContext().depthMask(GL_TRUE);
    getContext().clearDepthf(renderPass.depthAttachment.clearDepth);
  }
  if (renderPass.stencilAttachment.loadAction != LoadAction::Load) {
    clearMask |= GL_STENCIL_BUFFER_BIT;
    getContext().stencilMask(0xFF);
    getContext().clearStencil(renderPass.stencilAttachment.clearStencil);
  }

  if (clearMask != 0) {
    getContext().clear(clearMask);
  }
}

void CurrentFramebuffer::unbind() const {
  // no-op
}

FramebufferMode CurrentFramebuffer::getMode() const {
  return FramebufferMode::Mono;
}

} // namespace igl::opengl
