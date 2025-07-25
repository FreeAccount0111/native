/****************************************************************************
 Copyright (c) 2021 Xiamen Yaji Software Co., Ltd.
 
 http://www.cocos.com
 
 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated engine source code (the "Software"), a limited,
 worldwide, royalty-free, non-assignable, revocable and non-exclusive license
 to use Cocos Creator solely to develop games on your target platforms. You shall
 not use Cocos Creator software for developing other software or tools that's
 used for developing games. You are not granted to publish, distribute,
 sublicense, and/or sell copies of Cocos Creator.
 
 The software or tools in this License Agreement are licensed, not sold.
 Xiamen Yaji Software Co., Ltd. reserves all rights not expressly granted to you.
 
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#include "base/std/container/array.h"

// #include "core/Director.h"
#include "core/Root.h"
#include "core/TypedArray.h"
#include "core/assets/Material.h"
#include "core/event/EventTypesToJS.h"
#include "gfx-base/GFXTexture.h"
#include "profiler/Profiler.h"
#include "renderer/pipeline/Define.h"
#include "renderer/pipeline/InstancedBuffer.h"
#include "renderer/pipeline/custom/RenderInterfaceTypes.h"
#include "scene/Model.h"
#include "scene/Pass.h"
#include "scene/RenderScene.h"
#include "scene/SubModel.h"

namespace {

cc::Float32Array &vec4ToFloat32Array(const cc::Vec4 &v, cc::Float32Array &out, index_t ofs = 0) {
    out[ofs + 0] = v.x;
    out[ofs + 1] = v.y;
    out[ofs + 2] = v.z;
    out[ofs + 3] = v.w;
    return out;
}

cc::Float32Array &mat4ToFloat32Array(const cc::Mat4 &mat, cc::Float32Array &out, index_t ofs = 0) {
    memcpy(reinterpret_cast<float *>(const_cast<uint8_t *>(out.buffer()->getData())) + ofs, mat.m, 16 * sizeof(float));
    return out;
}

const cc::gfx::SamplerInfo LIGHTMAP_SAMPLER_HASH{
    cc::gfx::Filter::LINEAR,
    cc::gfx::Filter::LINEAR,
    cc::gfx::Filter::NONE,
    cc::gfx::Address::CLAMP,
    cc::gfx::Address::CLAMP,
    cc::gfx::Address::CLAMP,
};

const cc::gfx::SamplerInfo LIGHTMAP_SAMPLER_WITH_MIP_HASH{
    cc::gfx::Filter::LINEAR,
    cc::gfx::Filter::LINEAR,
    cc::gfx::Filter::LINEAR,
    cc::gfx::Address::CLAMP,
    cc::gfx::Address::CLAMP,
    cc::gfx::Address::CLAMP,
};

const ccstd::vector<cc::scene::IMacroPatch> SHADOW_MAP_PATCHES{{"CC_RECEIVE_SHADOW", true}};
} // namespace

namespace cc {
namespace scene {

Model::Model() {
    _device = Root::getInstance()->getDevice();
}

Model::~Model() = default;

void Model::initialize() {
    if (_inited) return;
    _receiveShadow = true;
    _castShadow = false;
    _enabled = true;
    _visFlags = Layers::Enum::NONE;
    _inited = true;
    _localData.reset(pipeline::UBOLocal::COUNT);
}

void Model::destroy() {
    for (SubModel *subModel : _subModels) {
        CC_SAFE_DESTROY(subModel);
    }
    _subModels.clear();

    CC_SAFE_DESTROY_NULL(_localBuffer);
    CC_SAFE_DESTROY_NULL(_worldBoundBuffer);

    _worldBounds = nullptr;
    _modelBounds = nullptr;
    _inited = false;
    _localDataUpdated = true;
    _transform = nullptr;
    _node = nullptr;
    _isDynamicBatching = false;
}

void Model::updateTransform(uint32_t stamp) {
    CC_PROFILE(ModelUpdateTransform);
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            _eventProcessor.emit(EventTypesToJS::MODEL_UPDATE_TRANSFORM, stamp);
            _isCalledFromJS = false;
            return;
        }
    }

    Node *node = _transform;
    if (node->getChangedFlags() || node->getDirtyFlag()) {
        node->updateWorldTransform();
        _localDataUpdated = true;
        if (_modelBounds != nullptr && _modelBounds->isValid() && _worldBounds != nullptr) {
            _modelBounds->transform(node->getWorldMatrix(), _worldBounds);
            _worldBoundsDirty = true;
        }
    }
}

void Model::updateWorldBound() {
    Node *node = _transform;
    if (node) {
        node->updateWorldTransform();
        _localDataUpdated = true;
        if (_modelBounds != nullptr && _modelBounds->isValid() && _worldBounds != nullptr) {
            _modelBounds->transform(node->getWorldMatrix(), _worldBounds);
            _worldBoundsDirty = true;
        }
    }
}

void Model::updateWorldBoundsForJSSkinningModel(const Vec3 &min, const Vec3 &max) {
    Node *node = _transform;
    if (node) {
        if (_modelBounds != nullptr && _modelBounds->isValid() && _worldBounds != nullptr) {
            geometry::AABB::fromPoints(min, max, _modelBounds);
            _modelBounds->transform(node->getWorldMatrix(), _worldBounds);
            _worldBoundsDirty = true;
        }
    }
}

void Model::updateWorldBoundsForJSBakedSkinningModel(geometry::AABB *aabb) {
    _worldBounds->center = aabb->center;
    _worldBounds->halfExtents = aabb->halfExtents;
    _worldBoundsDirty = true;
}

void Model::updateUBOs(uint32_t stamp) {
    CC_PROFILE(ModelUpdateUBOs);
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            _eventProcessor.emit(EventTypesToJS::MODEL_UPDATE_UBO, stamp);
            _isCalledFromJS = false;
            return;
        }
    }

    for (SubModel *subModel : _subModels) {
        subModel->update();
    }
    _updateStamp = stamp;

    const auto *pipeline = Root::getInstance()->getPipeline();
    const auto *shadowInfo = pipeline->getPipelineSceneData()->getShadows();
    const auto forceUpdateUBO = shadowInfo->isEnabled() && shadowInfo->getType() == ShadowType::PLANAR;

    if (!_localDataUpdated) {
        return;
    }
    _localDataUpdated = false;
    getTransform()->updateWorldTransform();
    const auto &worldMatrix = getTransform()->getWorldMatrix();
    bool hasNonInstancingPass = false;
    for (const auto &subModel : _subModels) {
        const auto idx = subModel->getInstancedWorldMatrixIndex();
        if (idx >= 0) {
            ccstd::vector<TypedArray> &attrs = subModel->getInstancedAttributeBlock().views;
            subModel->updateInstancedWorldMatrix(worldMatrix, idx);
        } else {
            hasNonInstancingPass = true;
        }
    }

    if ((hasNonInstancingPass || forceUpdateUBO) && _localBuffer) {
        Mat4 mat4;
        mat4ToFloat32Array(worldMatrix, _localData, pipeline::UBOLocal::MAT_WORLD_OFFSET);
        Mat4::inverseTranspose(worldMatrix, &mat4);

        mat4ToFloat32Array(mat4, _localData, pipeline::UBOLocal::MAT_WORLD_IT_OFFSET);
        _localBuffer->update(_localData.buffer()->getData());
        const bool enableOcclusionQuery = Root::getInstance()->getPipeline()->isOcclusionQueryEnabled();
        if (enableOcclusionQuery) {
            updateWorldBoundUBOs();
        }
    }
}

void Model::updateOctree() {
    if (_scene && _worldBoundsDirty) {
        _worldBoundsDirty = false;
        _scene->updateOctree(this);
    }
}

void Model::updateWorldBoundUBOs() {
    if (_worldBoundBuffer) {
        ccstd::array<float, pipeline::UBOWorldBound::COUNT> worldBoundBufferView;
        const Vec3 &center = _worldBounds ? _worldBounds->getCenter() : Vec3{0.0F, 0.0F, 0.0F};
        const Vec3 &halfExtents = _worldBounds ? _worldBounds->getHalfExtents() : Vec3{1.0F, 1.0F, 1.0F};
        const Vec4 worldBoundCenter{center.x, center.y, center.z, 0.0F};
        const Vec4 worldBoundHalfExtents{halfExtents.x, halfExtents.y, halfExtents.z, 1.0F};
        memcpy(worldBoundBufferView.data() + pipeline::UBOWorldBound::WORLD_BOUND_CENTER, &worldBoundCenter.x, sizeof(Vec4));
        memcpy(worldBoundBufferView.data() + pipeline::UBOWorldBound::WORLD_BOUND_HALF_EXTENTS, &worldBoundHalfExtents.x, sizeof(Vec4));
        _worldBoundBuffer->update(worldBoundBufferView.data(), pipeline::UBOWorldBound::SIZE);
    }
}

void Model::createBoundingShape(const ccstd::optional<Vec3> &minPos, const ccstd::optional<Vec3> &maxPos) {
    if (!minPos.has_value() || !maxPos.has_value()) {
        return;
    }

    if (!_modelBounds) {
        _modelBounds = ccnew geometry::AABB();
    }
    geometry::AABB::fromPoints(minPos.value(), maxPos.value(), _modelBounds);

    if (!_worldBounds) {
        _worldBounds = ccnew geometry::AABB();
    }
    geometry::AABB::fromPoints(minPos.value(), maxPos.value(), _worldBounds);
    _worldBoundsDirty = true;
}

SubModel *Model::createSubModel() {
    return ccnew SubModel();
}

void Model::initSubModel(index_t idx, cc::RenderingSubMesh *subMeshData, Material *mat) {
    initialize();
    if (idx >= static_cast<index_t>(_subModels.size())) {
        _subModels.resize(1 + idx, nullptr);
    }

    if (_subModels[idx] == nullptr) {
        _subModels[idx] = createSubModel();
    } else {
        CC_SAFE_DESTROY(_subModels[idx]);
    }
    _subModels[idx]->initialize(subMeshData, mat->getPasses(), getMacroPatches(idx));
    _subModels[idx]->initPlanarShadowShader();
    _subModels[idx]->initPlanarShadowInstanceShader();
    _subModels[idx]->setOwner(this);
    updateAttributesAndBinding(idx);
}

void Model::setSubModelMesh(index_t idx, cc::RenderingSubMesh *subMesh) const {
    if (idx < _subModels.size()) {
        _subModels[idx]->setSubMesh(subMesh);
    }
}

void Model::setSubModelMaterial(index_t idx, Material *mat) {
    if (idx < _subModels.size()) {
        _subModels[idx]->setPasses(mat->getPasses());
        updateAttributesAndBinding(idx);
    }
}

void Model::onGlobalPipelineStateChanged() const {
    for (SubModel *subModel : _subModels) {
        subModel->onPipelineStateChanged();
    }
}

void Model::onMacroPatchesStateChanged() {
    for (index_t i = 0; i < _subModels.size(); ++i) {
        _subModels[i]->onMacroPatchesStateChanged(getMacroPatches(i));
    }
}

void Model::onGeometryChanged() {
    for (SubModel *subModel : _subModels) {
        subModel->onGeometryChanged();
    }
}

void Model::initLightingmap(Texture2D *texture, const Vec4 &uvParam) {
    _lightmap = texture;
    _lightmapUVParam = uvParam;
}

void Model::updateLightingmap(Texture2D *texture, const Vec4 &uvParam) {
    vec4ToFloat32Array(uvParam, _localData, pipeline::UBOLocal::LIGHTINGMAP_UVPARAM); //TODO(xwx): toArray not implemented in Math
    _localDataUpdated = true;
    _lightmap = texture;
    _lightmapUVParam = uvParam;

    if (texture == nullptr) {
        texture = BuiltinResMgr::getInstance()->get<Texture2D>(ccstd::string("empty-texture"));
    }
    gfx::Texture *gfxTexture = texture->getGFXTexture();
    if (gfxTexture) {
        auto *sampler = _device->getSampler(texture->getMipmaps().size() > 1 ? LIGHTMAP_SAMPLER_WITH_MIP_HASH : LIGHTMAP_SAMPLER_HASH);
        for (SubModel *subModel : _subModels) {
            gfx::DescriptorSet *descriptorSet = subModel->getDescriptorSet();
            // // TODO(Yun Hsiao Wu): should manage lightmap macro switches automatically
            // // USE_LIGHTMAP -> CC_USE_LIGHTMAP
            descriptorSet->bindTexture(pipeline::LIGHTMAPTEXTURE::BINDING, gfxTexture);
            descriptorSet->bindSampler(pipeline::LIGHTMAPTEXTURE::BINDING, sampler);
            descriptorSet->update();
        }
    }
}

ccstd::vector<IMacroPatch> Model::getMacroPatches(index_t subModelIndex) {
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            ccstd::vector<IMacroPatch> macroPatches;
            _eventProcessor.emit(EventTypesToJS::MODEL_GET_MACRO_PATCHES, subModelIndex, &macroPatches);
            _isCalledFromJS = false;
            return macroPatches;
        }
    }

    if (_receiveShadow) {
        return SHADOW_MAP_PATCHES;
    }

    return {};
}

void Model::updateAttributesAndBinding(index_t subModelIndex) {
    if (subModelIndex >= _subModels.size()) return;
    SubModel *subModel = _subModels[subModelIndex];
    initLocalDescriptors(subModelIndex);
    updateLocalDescriptors(subModelIndex, subModel->getDescriptorSet());

    initWorldBoundDescriptors(subModelIndex);
    if (subModel->getWorldBoundDescriptorSet()) {
        updateWorldBoundDescriptors(subModelIndex, subModel->getWorldBoundDescriptorSet());
    }

    gfx::Shader *shader = subModel->getPasses()[0]->getShaderVariant(subModel->getPatches());
    updateInstancedAttributes(shader->getAttributes(), subModel);
}

void Model::updateInstancedAttributes(const ccstd::vector<gfx::Attribute> &attributes, SubModel* subModel) {
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            _eventProcessor.emit(EventTypesToJS::MODEL_UPDATE_INSTANCED_ATTRIBUTES, attributes, subModel);
            _isCalledFromJS = false;
            return;
        }
    }
    subModel->updateInstancedAttributes(attributes);
    _localDataUpdated = true;
}

void Model::initLocalDescriptors(index_t /*subModelIndex*/) {
    if (!_localBuffer) {
        _localBuffer = _device->createBuffer({
            gfx::BufferUsageBit::UNIFORM | gfx::BufferUsageBit::TRANSFER_DST,
            gfx::MemoryUsageBit::DEVICE,
            pipeline::UBOLocal::SIZE,
            pipeline::UBOLocal::SIZE,
        });
    }
}

void Model::initWorldBoundDescriptors(index_t /*subModelIndex*/) {
    if (!_worldBoundBuffer) {
        _worldBoundBuffer = _device->createBuffer({
            gfx::BufferUsageBit::UNIFORM | gfx::BufferUsageBit::TRANSFER_DST,
            gfx::MemoryUsageBit::DEVICE,
            pipeline::UBOLocal::SIZE,
            pipeline::UBOLocal::SIZE,
        });
    }
}

void Model::updateLocalDescriptors(index_t subModelIndex, gfx::DescriptorSet *descriptorSet) {
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            _eventProcessor.emit(EventTypesToJS::MODEL_UPDATE_LOCAL_DESCRIPTORS, subModelIndex, descriptorSet);
            _isCalledFromJS = false;
            return;
        }
    }

    if (_localBuffer) {
        descriptorSet->bindBuffer(pipeline::UBOLocal::BINDING, _localBuffer);
    }
}

void Model::updateWorldBoundDescriptors(index_t subModelIndex, gfx::DescriptorSet *descriptorSet) {
    if (isModelImplementedInJS()) {
        if (!_isCalledFromJS) {
            _eventProcessor.emit(EventTypesToJS::MODEL_UPDATE_LOCAL_DESCRIPTORS, subModelIndex, descriptorSet);
            _isCalledFromJS = false;
            return;
        }
    }

    if (_worldBoundBuffer) {
        descriptorSet->bindBuffer(pipeline::UBOLocal::BINDING, _worldBoundBuffer);
    }
}

void Model::updateLocalShadowBias() {
    _localData[pipeline::UBOLocal::LOCAL_SHADOW_BIAS + 0] = _shadowBias;
    _localData[pipeline::UBOLocal::LOCAL_SHADOW_BIAS + 1] = _shadowNormalBias;
    _localData[pipeline::UBOLocal::LOCAL_SHADOW_BIAS + 2] = 0;
    _localData[pipeline::UBOLocal::LOCAL_SHADOW_BIAS + 3] = 0;
    _localDataUpdated = true;
}

void Model::setInstancedAttribute(const ccstd::string &name, const float *value, uint32_t byteLength) {
    for (const auto &subModel : _subModels) {
        subModel->setInstancedAttribute(name, value, byteLength);
    }
}

} // namespace scene
} // namespace cc
