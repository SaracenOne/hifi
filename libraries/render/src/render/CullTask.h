//
//  CullTask.h
//  render/src/render
//
//  Created by Sam Gateau on 2/2/16.
//  Copyright 2016 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_render_CullTask_h
#define hifi_render_CullTask_h

#include "Engine.h"
#include "ViewFrustum.h"

namespace render {

    using CullFunctor = std::function<bool(const RenderArgs*, const AABox&)>;

    void cullItems(const RenderContextPointer& renderContext, const CullFunctor& cullFunctor, RenderDetails::Item& details,
        const ItemBounds& inItems, ItemBounds& outItems);
    void depthSortItems(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, bool frontToBack, const ItemBounds& inItems, ItemBounds& outItems);


    class FetchItemsConfig : public Job::Config {
        Q_OBJECT
        Q_PROPERTY(int numItems READ getNumItems)
        Q_PROPERTY(bool freezeFrustum MEMBER freezeFrustum WRITE setFreezeFrustum)
    public:
        int getNumItems() { return numItems; }

        int numItems{ 0 };

        bool freezeFrustum{ false };

    public slots:
        void setFreezeFrustum(bool enabled) { freezeFrustum = enabled; emit dirty(); }

    signals:
        void dirty();
    };

    class FetchItems {
        bool _freezeFrustum{ false }; // initialized by Config
        bool _justFrozeFrustum{ false };
        ViewFrustum _frozenFrutstum;
    public:
        using Config = FetchItemsConfig;
        using JobModel = Job::ModelO<FetchItems, ItemBounds, Config>;

        FetchItems() {}
        FetchItems(const ItemFilter& filter) : _filter(filter) {}

        ItemFilter _filter{ ItemFilter::Builder::opaqueShape().withoutLayered() };

        void configure(const Config& config);
        void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, ItemBounds& outItems);
    };


    template<RenderDetails::Type T>
    class CullItems {
    public:
        using JobModel = Job::ModelIO<CullItems<T>, ItemBounds, ItemBounds>;

        CullItems(CullFunctor cullFunctor) : _cullFunctor{ cullFunctor } {}

        void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemBounds& inItems, ItemBounds& outItems) {
            const auto& args = renderContext->args;
            auto& details = args->_details.edit(T);
            outItems.clear();
            outItems.reserve(inItems.size());
            render::cullItems(renderContext, _cullFunctor, details, inItems, outItems);
        }

    protected:
        CullFunctor _cullFunctor;
    };

    class DepthSortItems {
    public:
        using JobModel = Job::ModelIO<DepthSortItems, ItemBounds, ItemBounds>;

        bool _frontToBack;
        DepthSortItems(bool frontToBack = true) : _frontToBack(frontToBack) {}

        void run(const SceneContextPointer& sceneContext, const RenderContextPointer& renderContext, const ItemBounds& inItems, ItemBounds& outItems);
    };
}

#endif // hifi_render_CullTask_h