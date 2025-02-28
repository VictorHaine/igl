/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <shell/shared/fileLoader/FileLoader.h>
#include <shell/shared/imageLoader/ImageLoader.h>
#include <shell/shared/platform/Platform.h>

namespace igl::shell {

class PlatformAndroid : public Platform {
 public:
  PlatformAndroid(std::unique_ptr<igl::IDevice> device, bool useFakeLoader = false);
  igl::IDevice& getDevice() noexcept override;
  virtual std::shared_ptr<igl::IDevice> getDevicePtr() const noexcept override;
  ImageLoader& getImageLoader() noexcept override;
  const ImageWriter& getImageWriter() const noexcept override;
  FileLoader& getFileLoader() const noexcept override;

 private:
  std::shared_ptr<igl::IDevice> device_;
  std::shared_ptr<FileLoader> fileLoader_;
  std::shared_ptr<ImageWriter> imageWriter_;
  std::shared_ptr<ImageLoader> imageLoader_;
};

} // namespace igl::shell
