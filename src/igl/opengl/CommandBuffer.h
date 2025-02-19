/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <igl/CommandBuffer.h>

namespace igl {
namespace opengl {
class ComputeCommandEncoder;
class IContext;
class PipelineState;
class RenderCommandEncoder;
class Texture;

class CommandBuffer final : public ICommandBuffer,
                            public std::enable_shared_from_this<CommandBuffer> {
 public:
  explicit CommandBuffer(std::shared_ptr<IContext> context);
  ~CommandBuffer() override;

  std::unique_ptr<IRenderCommandEncoder> createRenderCommandEncoder(
      const RenderPassDesc& renderPass,
      std::shared_ptr<IFramebuffer> framebuffer,
      const Dependencies& dependencies,
      Result* outResult) override;

  std::unique_ptr<IComputeCommandEncoder> createComputeCommandEncoder() override;

  void present(std::shared_ptr<ITexture> surface) const override;

  void waitUntilScheduled() override;

  void waitUntilCompleted() override;

  void pushDebugGroupLabel(const std::string& label, const igl::Color& color) const override;

  void popDebugGroupLabel() const override;

  IContext& getContext() const;

 private:
  std::shared_ptr<IContext> context_;
};

} // namespace opengl
} // namespace igl
