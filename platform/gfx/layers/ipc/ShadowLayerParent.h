/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_ShadowLayerParent_h
#define mozilla_layers_ShadowLayerParent_h

#include "mozilla/Attributes.h"         // for override
#include "mozilla/ipc/ProtocolUtils.h"
#include "mozilla/layers/PLayerParent.h"  // for PLayerParent

namespace mozilla {
namespace layers {

class ContainerLayer;
class Layer;

class CanvasLayerComposite;
class ColorLayerComposite;
class ContainerLayerComposite;
class ImageLayerComposite;
class RefLayerComposite;
class PaintedLayerComposite;

class ShadowLayerParent : public PLayerParent
{
public:
  ShadowLayerParent();

  virtual ~ShadowLayerParent();

  void Bind(Layer* layer);
  void Destroy();

  Layer* AsLayer() const { return mLayer; }

  ContainerLayerComposite* AsContainerLayerComposite() const;
  CanvasLayerComposite* AsCanvasLayerComposite() const;
  ColorLayerComposite* AsColorLayerComposite() const;
  ImageLayerComposite* AsImageLayerComposite() const;
  RefLayerComposite* AsRefLayerComposite() const;
  PaintedLayerComposite* AsPaintedLayerComposite() const;

private:
  virtual void ActorDestroy(ActorDestroyReason why) override;

  void Disconnect();

  RefPtr<Layer> mLayer;
};

} // namespace layers
} // namespace mozilla

#endif // ifndef mozilla_layers_ShadowLayerParent_h
