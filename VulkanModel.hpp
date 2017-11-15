/*
* Vulkan Model loader using ASSIMP
*
* Copyright(C) 2016-2017 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license(MIT) (http://opensource.org/licenses/MIT)
*/
#pragma once

#include <stdlib.h>
#include <string>
#include <fstream>
#include <vector>

#include "vulkan/vulkan.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/cimport.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "VulkanDevice.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanTexture.hpp"

#if defined(__ANDROID__)
#include <android/asset_manager.h>
#endif

namespace vks
{
    struct Material {
        glm::vec4 Ka;
        glm::vec4 Kd;
        glm::vec4 Ks;
        glm::vec4 Ke;
        uint32_t Ma;
        uint32_t Md;
        uint32_t Me;
        float Ns;
        float Ni;
        float d;
        float pad1;
        float pad2;
    };

    /** @brief Vertex layout components */
    typedef enum Component {
        VERTEX_COMPONENT_POSITION = 0x0,
        VERTEX_COMPONENT_NORMAL = 0x1,
        VERTEX_COMPONENT_COLOR = 0x2,
        VERTEX_COMPONENT_UV = 0x3,
        VERTEX_COMPONENT_TANGENT = 0x4,
        VERTEX_COMPONENT_BITANGENT = 0x5,
        VERTEX_COMPONENT_DUMMY_FLOAT = 0x6,
        VERTEX_COMPONENT_DUMMY_VEC4 = 0x7
    } Component;

    /** @brief Stores vertex layout components for model loading and Vulkan vertex input and atribute bindings  */
    struct VertexLayout {
    public:
        /** @brief Components used to generate vertices from */
        std::vector<Component> components;

        VertexLayout(std::vector<Component> components)
        {
            this->components = std::move(components);
        }

        uint32_t stride()
        {
            uint32_t res = 0;
            for (auto& component : components)
            {
                switch (component)
                {
                case VERTEX_COMPONENT_UV:
                    res += 2 * sizeof(float);
                    break;
                case VERTEX_COMPONENT_DUMMY_FLOAT:
                    res += sizeof(float);
                    break;
                case VERTEX_COMPONENT_DUMMY_VEC4:
                    res += 4 * sizeof(float);
                    break;
                default:
                    // All components except the ones listed above are made up of 3 floats
                    res += 3 * sizeof(float);
                }
            }
            return res;
        }
    };

    /** @brief Used to parametrize model loading */
    struct ModelCreateInfo {
        glm::vec3 center;
        glm::vec3 scale;
        glm::vec2 uvscale;

        ModelCreateInfo() {}

        ModelCreateInfo(glm::vec3 scale, glm::vec2 uvscale, glm::vec3 center)
        {
            this->center = center;
            this->scale = scale;
            this->uvscale = uvscale;
        }

        ModelCreateInfo(float scale, float uvscale, float center)
        {
            this->center = glm::vec3(center);
            this->scale = glm::vec3(scale);
            this->uvscale = glm::vec2(uvscale);
        }

    };

    struct Model {
        VkDevice device = nullptr;
        vks::Buffer vertices;
        vks::Buffer indices;
        uint32_t indexCount = 0;
        uint32_t vertexCount = 0;
        std::vector<Material> materials;
        vks::Texture2DArray texArray;

        /** @brief Stores vertex and index base and counts for each part of a model */
        struct ModelPart {
            uint32_t vertexBase;
            uint32_t vertexCount;
            uint32_t indexBase;
            uint32_t indexCount;
            uint32_t materialIdx;
        };
        std::vector<ModelPart> parts;

        /*static const int defaultFlags = aiProcess_Triangulate | aiProcess_PreTransformVertices |
                aiProcess_OptimizeMeshes | aiProcess_FlipWindingOrder |
                aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;*/
        static const int defaultFlags =  aiProcess_MakeLeftHanded | aiProcess_OptimizeMeshes |
                aiProcess_Triangulate | aiProcess_JoinIdenticalVertices| aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;

        //static const int defaultFlags =  aiProcess_MakeLeftHanded | aiProcess_OptimizeMeshes |
        //        aiProcess_Triangulate | aiProcess_JoinIdenticalVertices| aiProcess_CalcTangentSpace | aiProcess_GenSmoothNormals;

        struct Dimension
        {
            glm::vec3 min = glm::vec3(FLT_MAX);
            glm::vec3 max = glm::vec3(-FLT_MAX);
            glm::vec3 size;
        } dim;

        /** @brief Release all Vulkan resources of this model */
        void destroy()
        {
            assert(device);

            texArray.destroy();
            vkDestroyBuffer(device, vertices.buffer, nullptr);
            vkFreeMemory(device, vertices.memory, nullptr);
            if (indices.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(device, indices.buffer, nullptr);
                vkFreeMemory(device, indices.memory, nullptr);
            }
        }
        static bool compareNoCase( const std::string& s1, const std::string& s2 ) {
            return strcasecmp( s1.c_str(), s2.c_str() ) <= 0;
        }
        /**
        * Loads a 3D model from a file into Vulkan buffers
        *
        * @param device Pointer to the Vulkan device used to generated the vertex and index buffers on
        * @param filename File to load (must be a model format supported by ASSIMP)
        * @param layout Vertex layout components (position, normals, tangents, etc.)
        * @param createInfo MeshCreateInfo structure for load time settings like scale, center, etc.
        * @param copyQueue Queue used for the memory staging copy commands (must support transfer)
        * @param (Optional) flags ASSIMP model loading flags
        */
        bool loadFromFile(const std::string& filename, vks::VertexLayout layout, vks::ModelCreateInfo *createInfo, vks::VulkanDevice *device, VkQueue copyQueue, const int flags = defaultFlags)
        {
            this->device = device->logicalDevice;

            Assimp::Importer Importer;

            const aiScene* pScene;

            // Load file
#if defined(__ANDROID__)
            // Meshes are stored inside the apk on Android (compressed)
            // So they need to be loaded via the asset manager

            AAsset* asset = AAssetManager_open(androidApp->activity->assetManager, filename.c_str(), AASSET_MODE_STREAMING);
            if (!asset) {
                LOGE("Could not load mesh from \"%s\"!", filename.c_str());
                return false;
            }
            assert(asset);
            size_t size = AAsset_getLength(asset);

            assert(size > 0);

            void *meshData = malloc(size);
            AAsset_read(asset, meshData, size);
            AAsset_close(asset);

            pScene = Importer.ReadFileFromMemory(meshData, size, flags);

            free(meshData);
#else
            pScene = Importer.ReadFile(filename.c_str(), flags);
#endif

            if (pScene)
            {

                //build sorted dic of maps
                std::vector<std::string> mapDic;
                for (unsigned int i = 0; i < pScene->mNumMaterials; i++)
                {
                    const aiMaterial* mat = pScene->mMaterials[i];
                    aiString Md;
                    mat->GetTexture(aiTextureType_DIFFUSE, 0, &Md);
                    if (Md.length==0)
                        continue;
                    std::string map = std::string(Md.data);
                    auto it = std::find(mapDic.begin(), mapDic.end(), map);
                    if (it != mapDic.end())
                        continue;
                    mapDic.push_back(map);
                }

                if (mapDic.size()>0){
                    uint32_t texSize = 1024;

                    //create texture array
                    texArray.format = VK_FORMAT_R8G8B8A8_UNORM;
                    texArray.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    texArray.layerCount = mapDic.size();
                    texArray.width = texArray.height = texSize;
                    texArray.mipLevels = floor(log2(std::max(texArray.width, texArray.height))) + 1;
                    texArray.device = device;

                    VkImageCreateInfo imageCreateInfo = vks::initializers::imageCreateInfo();
                    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
                    imageCreateInfo.format = texArray.format;
                    imageCreateInfo.mipLevels = texArray.mipLevels;
                    imageCreateInfo.arrayLayers = texArray.layerCount;
                    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
                    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
                    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                    imageCreateInfo.extent = { texArray.width, texArray.height, 1 };
                    imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
                    VK_CHECK_RESULT(vkCreateImage(device->logicalDevice, &imageCreateInfo, nullptr, &texArray.image));

                    VkMemoryRequirements memReqs = {};
                    VkMemoryAllocateInfo memAllocInfo = vks::initializers::memoryAllocateInfo();
                    vkGetImageMemoryRequirements(device->logicalDevice, texArray.image, &memReqs);
                    memAllocInfo.allocationSize = memReqs.size;
                    memAllocInfo.memoryTypeIndex = device->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
                    VK_CHECK_RESULT(vkAllocateMemory(device->logicalDevice, &memAllocInfo, nullptr, &texArray.deviceMemory));
                    VK_CHECK_RESULT(vkBindImageMemory(device->logicalDevice, texArray.image, texArray.deviceMemory, 0));

                    for (int l = 0; l < mapDic.size(); l++) {
                        Texture inTex;
                        inTex.loadStbLinearNoSampling(mapDic[l].c_str(), device);

                        VkCommandBuffer blitFirstMipCmd = device->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);
                        VkImageBlit firstMipBlit{};

                        // Source
                        firstMipBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        firstMipBlit.srcSubresource.layerCount = 1;
                        firstMipBlit.srcSubresource.mipLevel = 0;
                        firstMipBlit.srcSubresource.baseArrayLayer = 0;
                        firstMipBlit.srcOffsets[1].x = inTex.width;
                        firstMipBlit.srcOffsets[1].y = inTex.height;
                        firstMipBlit.srcOffsets[1].z = 1;

                        // Destination
                        firstMipBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        firstMipBlit.dstSubresource.layerCount = 1;
                        firstMipBlit.dstSubresource.mipLevel = 0;
                        firstMipBlit.dstSubresource.baseArrayLayer = l;
                        firstMipBlit.dstOffsets[1].x = texArray.width;
                        firstMipBlit.dstOffsets[1].y = texArray.height;
                        firstMipBlit.dstOffsets[1].z = 1;

                        VkImageSubresourceRange firstMipSubRange = {};
                        firstMipSubRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                        firstMipSubRange.baseArrayLayer = l;
                        firstMipSubRange.levelCount = 1;
                        firstMipSubRange.layerCount = 1;

                        // Transiton current array level to transfer dest
                        vks::tools::setImageLayout(
                            blitFirstMipCmd,
                            texArray.image,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            firstMipSubRange,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_HOST_BIT);

                        // Blit from source texture
                        vkCmdBlitImage(
                            blitFirstMipCmd,
                            inTex.image,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            texArray.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1,
                            &firstMipBlit,
                            VK_FILTER_LINEAR);

                        //set layer ready for sampling
                        vks::tools::setImageLayout(
                            blitFirstMipCmd,
                            texArray.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            texArray.imageLayout,
                            firstMipSubRange);

                        device->flushCommandBuffer(blitFirstMipCmd, copyQueue, true);

                        vkDestroyImage(device->logicalDevice, inTex.image, nullptr);
                        vkFreeMemory(device->logicalDevice, inTex.deviceMemory, nullptr);

                        VkCommandBuffer blitCmd = device->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

                        // Copy down mips from n-1 to n
                        for (int32_t i = 1; i < texArray.mipLevels; i++)
                        {
                            VkImageBlit imageBlit{};

                            // Source
                            imageBlit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            imageBlit.srcSubresource.layerCount = 1;
                            imageBlit.srcSubresource.mipLevel = i-1;
                            imageBlit.srcSubresource.baseArrayLayer = l;
                            imageBlit.srcOffsets[1].x = int32_t(texArray.width >> (i - 1));
                            imageBlit.srcOffsets[1].y = int32_t(texArray.height >> (i - 1));
                            imageBlit.srcOffsets[1].z = 1;

                            // Destination
                            imageBlit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            imageBlit.dstSubresource.layerCount = 1;
                            imageBlit.dstSubresource.mipLevel = i;
                            imageBlit.dstSubresource.baseArrayLayer = l;
                            imageBlit.dstOffsets[1].x = int32_t(texArray.width >> i);
                            imageBlit.dstOffsets[1].y = int32_t(texArray.height >> i);
                            imageBlit.dstOffsets[1].z = 1;

                            VkImageSubresourceRange mipSubRange = {};
                            mipSubRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                            mipSubRange.baseMipLevel = i;
                            mipSubRange.baseArrayLayer = l;
                            mipSubRange.levelCount = 1;
                            mipSubRange.layerCount = 1;

                            // Transiton current mip level to transfer dest
                            vks::tools::setImageLayout(
                                blitCmd,
                                texArray.image,
                                VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                mipSubRange,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_HOST_BIT);

                            // Blit from previous level
                            vkCmdBlitImage(
                                blitCmd,
                                texArray.image,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                texArray.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1,
                                &imageBlit,
                                VK_FILTER_LINEAR);

                            // Transiton current mip level to transfer source for read in next iteration
                            vks::tools::setImageLayout(
                                blitCmd,
                                texArray.image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                mipSubRange,
                                VK_PIPELINE_STAGE_HOST_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
                        }
                        //set layer ready for sampling
                        firstMipSubRange.levelCount = texArray.mipLevels;
                        vks::tools::setImageLayout(
                            blitFirstMipCmd,
                            texArray.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            texArray.imageLayout,
                            firstMipSubRange);
                        device->flushCommandBuffer(blitCmd, copyQueue, true);
                    }

                    // Create samplers
                    VkSamplerCreateInfo samplerCI = vks::initializers::samplerCreateInfo();
                    samplerCI.magFilter = VK_FILTER_LINEAR;
                    samplerCI.minFilter = VK_FILTER_LINEAR;
                    samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                    samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                    samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                    samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                    samplerCI.mipLodBias = 0.0f;
                    samplerCI.compareOp = VK_COMPARE_OP_NEVER;
                    samplerCI.minLod = 0.0f;
                    samplerCI.maxLod = (float)texArray.mipLevels;
                    samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
                    if (device->features.samplerAnisotropy)
                    {
                        samplerCI.maxAnisotropy = device->properties.limits.maxSamplerAnisotropy;
                        samplerCI.anisotropyEnable = VK_TRUE;
                    }else{
                        samplerCI.maxAnisotropy = 1.0;
                        samplerCI.anisotropyEnable = VK_FALSE;
                    }
                    VK_CHECK_RESULT(vkCreateSampler(device->logicalDevice, &samplerCI, nullptr, &texArray.sampler));

                    // Create image view
                    VkImageViewCreateInfo viewCI = vks::initializers::imageViewCreateInfo();
                    viewCI.image = texArray.image;
                    viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
                    viewCI.format = texArray.format;
                    viewCI.components = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
                    viewCI.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    viewCI.subresourceRange.baseMipLevel = 0;
                    viewCI.subresourceRange.baseArrayLayer = 0;
                    viewCI.subresourceRange.layerCount = texArray.layerCount;
                    viewCI.subresourceRange.levelCount = texArray.mipLevels;
                    VK_CHECK_RESULT(vkCreateImageView(device->logicalDevice, &viewCI, nullptr, &texArray.view));
                    texArray.updateDescriptor();
                }

                parts.clear();
                parts.resize(pScene->mNumMeshes);

                glm::vec3 scale(1.0f);
                glm::vec2 uvscale(1.0f);
                glm::vec3 center(0.0f);
                if (createInfo)
                {
                    scale = createInfo->scale;
                    uvscale = createInfo->uvscale;
                    center = createInfo->center;
                }

                std::vector<float> vertexBuffer;
                std::vector<uint32_t> indexBuffer;

                vertexCount = 0;
                indexCount = 0;

                // Load materials
                materials.clear();
                materials.resize(pScene->mNumMaterials);
                for (unsigned int i = 0; i < pScene->mNumMaterials; i++)
                {
                    const aiMaterial* mat = pScene->mMaterials[i];

                    aiColor3D pColor(0.f, 0.f, 0.f);
                    mat->Get(AI_MATKEY_COLOR_AMBIENT, pColor);
                    materials[i].Ka.x = pColor.r;
                    materials[i].Ka.y = pColor.g;
                    materials[i].Ka.z = pColor.b;
                    mat->Get(AI_MATKEY_COLOR_DIFFUSE, pColor);
                    materials[i].Kd.x = pColor.r;
                    materials[i].Kd.y = pColor.g;
                    materials[i].Kd.z = pColor.b;
                    mat->Get(AI_MATKEY_COLOR_SPECULAR, pColor);
                    materials[i].Ks.x = pColor.r;
                    materials[i].Ks.y = pColor.g;
                    materials[i].Ks.z = pColor.b;
                    mat->Get(AI_MATKEY_COLOR_EMISSIVE, pColor);
                    materials[i].Ke.x = pColor.r;
                    materials[i].Ke.y = pColor.g;
                    materials[i].Ke.z = pColor.b;

                    mat->Get(AI_MATKEY_SHININESS, materials[i].Ns);


                    aiString mapPath;
//					mat->Get(AI_MATKEY_MAPPING_DIFFUSE, pColor);
//					pScene->mMaterials[paiMesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, pColor);
//					aiString name;
//					pScene->mMaterials[paiMesh->mMaterialIndex]->Get(AI_MATKEY_NAME,name);

                    mat->GetTexture(aiTextureType_DIFFUSE, 0, &mapPath);
                    std::string mapFile = std::string(mapPath.data);
                    auto it = std::find(mapDic.begin(), mapDic.end(), mapFile);
                    if (it != mapDic.end()){
                        materials[i].Md = std::distance(mapDic.begin(), it);
                        std::cout << "material: " << i <<  " dds-idx: " << std::distance(mapDic.begin(), it) << " -> " << mapPath.data << std::endl;
                    }
                }

                // Load meshes
                for (unsigned int i = 0; i < pScene->mNumMeshes; i++)
                {
                    const aiMesh* paiMesh = pScene->mMeshes[i];

                    parts[i] = {};
                    parts[i].vertexBase = vertexCount;
                    parts[i].indexBase = indexCount;
                    parts[i].materialIdx = paiMesh->mMaterialIndex;

                    vertexCount += paiMesh->mNumVertices;

                    const aiVector3D Zero3D(0.0f, 0.0f, 0.0f);

                    aiColor3D pColor(0.f, 0.f, 0.f);
                    pScene->mMaterials[paiMesh->mMaterialIndex]->Get(AI_MATKEY_COLOR_DIFFUSE, pColor);

                    for (unsigned int j = 0; j < paiMesh->mNumVertices; j++)
                    {
                        const aiVector3D* pPos = &(paiMesh->mVertices[j]);
                        const aiVector3D* pNormal = &(paiMesh->mNormals[j]);
                        const aiVector3D* pTexCoord = (paiMesh->HasTextureCoords(0)) ? &(paiMesh->mTextureCoords[0][j]) : &Zero3D;
                        const aiVector3D* pTangent = (paiMesh->HasTangentsAndBitangents()) ? &(paiMesh->mTangents[j]) : &Zero3D;
                        const aiVector3D* pBiTangent = (paiMesh->HasTangentsAndBitangents()) ? &(paiMesh->mBitangents[j]) : &Zero3D;

                        for (auto& component : layout.components)
                        {
                            switch (component) {
                            case VERTEX_COMPONENT_POSITION:
                                vertexBuffer.push_back(pPos->x * scale.x + center.x);
                                vertexBuffer.push_back(-pPos->y * scale.y + center.y);
                                vertexBuffer.push_back(pPos->z * scale.z + center.z);
                                break;
                            case VERTEX_COMPONENT_NORMAL:
                                vertexBuffer.push_back(pNormal->x);
                                vertexBuffer.push_back(-pNormal->y);
                                vertexBuffer.push_back(pNormal->z);
                                break;
                            case VERTEX_COMPONENT_UV:
                                vertexBuffer.push_back(pTexCoord->x * uvscale.s);
                                vertexBuffer.push_back(pTexCoord->y * uvscale.t);
                                break;
                            case VERTEX_COMPONENT_COLOR:
                                vertexBuffer.push_back(pColor.r);
                                vertexBuffer.push_back(pColor.g);
                                vertexBuffer.push_back(pColor.b);
                                break;
                            case VERTEX_COMPONENT_TANGENT:
                                vertexBuffer.push_back(pTangent->x);
                                vertexBuffer.push_back(pTangent->y);
                                vertexBuffer.push_back(pTangent->z);
                                break;
                            case VERTEX_COMPONENT_BITANGENT:
                                vertexBuffer.push_back(pBiTangent->x);
                                vertexBuffer.push_back(pBiTangent->y);
                                vertexBuffer.push_back(pBiTangent->z);
                                break;
                            // Dummy components for padding
                            case VERTEX_COMPONENT_DUMMY_FLOAT:
                                vertexBuffer.push_back(0.0f);
                                break;
                            case VERTEX_COMPONENT_DUMMY_VEC4:
                                vertexBuffer.push_back(0.0f);
                                vertexBuffer.push_back(0.0f);
                                vertexBuffer.push_back(0.0f);
                                vertexBuffer.push_back(0.0f);
                                break;
                            };
                        }

                        dim.max.x = fmax(pPos->x, dim.max.x);
                        dim.max.y = fmax(pPos->y, dim.max.y);
                        dim.max.z = fmax(pPos->z, dim.max.z);

                        dim.min.x = fmin(pPos->x, dim.min.x);
                        dim.min.y = fmin(pPos->y, dim.min.y);
                        dim.min.z = fmin(pPos->z, dim.min.z);
                    }

                    dim.size = dim.max - dim.min;

                    parts[i].vertexCount = paiMesh->mNumVertices;

                    uint32_t indexBase = static_cast<uint32_t>(indexBuffer.size());
                    for (unsigned int j = 0; j < paiMesh->mNumFaces; j++)
                    {
                        const aiFace& Face = paiMesh->mFaces[j];
                        if (Face.mNumIndices != 3)
                            continue;
                        indexBuffer.push_back(Face.mIndices[0]);
                        indexBuffer.push_back(Face.mIndices[1]);
                        indexBuffer.push_back(Face.mIndices[2]);
                        parts[i].indexCount += 3;
                        indexCount += 3;
                    }
                }


                uint32_t vBufferSize = static_cast<uint32_t>(vertexBuffer.size()) * sizeof(float);
                uint32_t iBufferSize = static_cast<uint32_t>(indexBuffer.size()) * sizeof(uint32_t);

                // Use staging buffer to move vertex and index buffer to device local memory
                // Create staging buffers
                vks::Buffer vertexStaging, indexStaging;

                // Vertex buffer
                VK_CHECK_RESULT(device->createBuffer(
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    &vertexStaging,
                    vBufferSize,
                    vertexBuffer.data()));

                // Index buffer
                VK_CHECK_RESULT(device->createBuffer(
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                    &indexStaging,
                    iBufferSize,
                    indexBuffer.data()));

                // Create device local target buffers
                // Vertex buffer
                VK_CHECK_RESULT(device->createBuffer(
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    &vertices,
                    vBufferSize));

                // Index buffer
                VK_CHECK_RESULT(device->createBuffer(
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                    &indices,
                    iBufferSize));

                // Copy from staging buffers
                VkCommandBuffer copyCmd = device->createCommandBuffer(VK_COMMAND_BUFFER_LEVEL_PRIMARY, true);

                VkBufferCopy copyRegion{};

                copyRegion.size = vertices.size;
                vkCmdCopyBuffer(copyCmd, vertexStaging.buffer, vertices.buffer, 1, &copyRegion);

                copyRegion.size = indices.size;
                vkCmdCopyBuffer(copyCmd, indexStaging.buffer, indices.buffer, 1, &copyRegion);

                device->flushCommandBuffer(copyCmd, copyQueue);

                // Destroy staging resources
                vkDestroyBuffer(device->logicalDevice, vertexStaging.buffer, nullptr);
                vkFreeMemory(device->logicalDevice, vertexStaging.memory, nullptr);
                vkDestroyBuffer(device->logicalDevice, indexStaging.buffer, nullptr);
                vkFreeMemory(device->logicalDevice, indexStaging.memory, nullptr);

                return true;
            }
            else
            {
                printf("Error parsing '%s': '%s'\n", filename.c_str(), Importer.GetErrorString());
#if defined(__ANDROID__)
                LOGE("Error parsing '%s': '%s'", filename.c_str(), Importer.GetErrorString());
#endif
                return false;
            }
        };

        /**
        * Loads a 3D model from a file into Vulkan buffers
        *
        * @param device Pointer to the Vulkan device used to generated the vertex and index buffers on
        * @param filename File to load (must be a model format supported by ASSIMP)
        * @param layout Vertex layout components (position, normals, tangents, etc.)
        * @param scale Load time scene scale
        * @param copyQueue Queue used for the memory staging copy commands (must support transfer)
        * @param (Optional) flags ASSIMP model loading flags
        */
        bool loadFromFile(const std::string& filename, vks::VertexLayout layout, float scale, vks::VulkanDevice *device, VkQueue copyQueue, const int flags = defaultFlags)
        {
            vks::ModelCreateInfo modelCreateInfo(scale, 1.0f, 0.0f);
            return loadFromFile(filename, layout, &modelCreateInfo, device, copyQueue, flags);
        }
    };
};
