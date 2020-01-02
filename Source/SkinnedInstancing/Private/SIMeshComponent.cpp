#include "SIMeshComponent.h"
#include "Classes/Materials/Material.h"
#include "PrimitiveSceneProxy.h"
#include "SkeletalMeshTypes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Animation/AnimSequence.h"
#include "BonePose.h"
#include "SIAnimationData.h"
#include "MeshDrawShaderBindings.h"

#pragma optimize( "", off )
namespace
{
	static TAutoConsoleVariable<int32> CVarSkinnedInstancingLimit2BoneInfluences(
		TEXT("r.SkinnedInstancing.Limit2BoneInfluences"),
		0,
		TEXT("Whether to use 2 bones influence instead of default 4 for GPU skinning. Cannot be changed at runtime."),
		ECVF_ReadOnly);

	static TAutoConsoleVariable<int32> CVarSkinnedInstancingDisableAnimationBlend(
		TEXT("r.SkinnedInstancing.DisableAnimationBlend"),
		0,
		TEXT("Whether to use animation blend. Cannot be changed at runtime."),
		ECVF_ReadOnly);

	static TAutoConsoleVariable<int32> CVarSkinnedInstancingDisableFrameLerp(
		TEXT("r.SkinnedInstancing.DisableFrameLerp"),
		0,
		TEXT("Whether to use frame lerp. Cannot be changed at runtime."),
		ECVF_ReadOnly);

	struct FVertexFactoryBuffers
	{
		FStaticMeshVertexBuffers* StaticVertexBuffers = nullptr;
		FSkinWeightVertexBuffer* SkinWeightVertexBuffer = nullptr;
		uint32 NumVertices = 0;
	};

	struct FVertexBufferAndSRV
	{
		void SafeRelease()
		{
			VertexBufferRHI.SafeRelease();
			VertexBufferSRV.SafeRelease();
		}

		bool IsValid()
		{
			return IsValidRef(VertexBufferRHI) && IsValidRef(VertexBufferSRV);
		}

		FVertexBufferRHIRef VertexBufferRHI;
		FShaderResourceViewRHIRef VertexBufferSRV;
	};

	class FGPUSkinVertexFactory : public FVertexFactory
	{
	public:
		FGPUSkinVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, uint32 InNumVertices)
			: FVertexFactory(InFeatureLevel)
			, NumVertices(InNumVertices)
		{

		}
	public:
		virtual void InitRHI() override;
		virtual void InitDynamicRHI() override;
		virtual void ReleaseDynamicRHI() override;
	public:
		enum
		{
			HasExtraBoneInfluences = false,
		};
	public:
		static FVertexFactoryType StaticType;
		virtual FVertexFactoryType* GetType() const override;

		struct FDataType : public FStaticMeshDataType
		{
			FVertexStreamComponent BoneIndices;
			FVertexStreamComponent BoneWeights;
		};

		struct FShaderDataType
		{
			void Release()
			{
				ensure(IsInRenderingThread());
				BoneMap.SafeRelease();
				RefBasesInvMatrix.SafeRelease();
				InstanceTransformBuffer.SafeRelease();
				InstanceAnimationBuffer.SafeRelease();
			}

			const FVertexBufferAndSRV& GetBoneMapForReading() const
			{
				return BoneMap;
			}

			const FVertexBufferAndSRV& GetRefBasesInvMatrixForReading() const
			{
				return RefBasesInvMatrix;
			}

			const FShaderResourceViewRHIRef& GetBoneBufferForReading() const
			{
				return BoneData->GetSRVForReading();
			}

			const FVertexBufferAndSRV& GetInstanceTransformBufferForReading() const
			{
				return InstanceTransformBuffer;
			}

			const FVertexBufferAndSRV& GetInstanceAnimationBufferForReading() const
			{
				return InstanceAnimationBuffer;
			}

			bool UpdateBoneMap(const TArray<FBoneIndexType>& _BoneMap)
			{
				uint32 BufferSize = _BoneMap.Num() * sizeof(uint32);

				if (!BoneMap.IsValid())
				{
					FRHIResourceCreateInfo CreateInfo;
					BoneMap.VertexBufferRHI = RHICreateVertexBuffer(BufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					BoneMap.VertexBufferSRV = RHICreateShaderResourceView(BoneMap.VertexBufferRHI, sizeof(uint32), PF_R32_UINT);
				}

				if (BoneMap.IsValid() && _BoneMap.Num() > 0)
				{
					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
					uint32* LockedBuffer = (uint32*)RHILockVertexBuffer(BoneMap.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					for (size_t i = 0; i < _BoneMap.Num(); i++)
					{
						LockedBuffer[i] = _BoneMap[i];
					}

					RHIUnlockVertexBuffer(BoneMap.VertexBufferRHI);
				}

				return true;
			}

			bool UpdateRefBasesInvMatrix(const TArray<FMatrix>& BoneMatrices)
			{
				uint32 BufferSize = BoneMatrices.Num() * 3 * sizeof(FVector4);

				if (!RefBasesInvMatrix.IsValid())
				{
					FRHIResourceCreateInfo CreateInfo;
					RefBasesInvMatrix.VertexBufferRHI = RHICreateVertexBuffer(BufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					RefBasesInvMatrix.VertexBufferSRV = RHICreateShaderResourceView(RefBasesInvMatrix.VertexBufferRHI, sizeof(FVector4), PF_A32B32G32R32F);
				}

				if (RefBasesInvMatrix.IsValid() && BoneMatrices.Num() > 0)
				{
					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
					FMatrix3x4* LockedBuffer = (FMatrix3x4*)RHILockVertexBuffer(RefBasesInvMatrix.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					for (size_t i = 0; i < BoneMatrices.Num(); i++)
					{
						FPlatformMisc::Prefetch(BoneMatrices.GetData() + i + PreFetchStride);
						FPlatformMisc::Prefetch(BoneMatrices.GetData() + i + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

						FMatrix3x4& BoneMat = LockedBuffer[i];
						const FMatrix& RefToLocal = BoneMatrices[i];
						RefToLocal.To3x4MatrixTranspose((float*)BoneMat.M);
					}

					RHIUnlockVertexBuffer(RefBasesInvMatrix.VertexBufferRHI);
				}

				return true;
			}

			void UpdateBoneData(const FSIAnimationData* BoneData)
			{
				this->BoneData = BoneData;
			}

			bool UpdateInstanceData(const TArray<FSIMeshInstanceData>& InstanceData, int MaxNumInstances)
			{
				const uint32 NumInstances = InstanceData.Num();
				uint32 BufferSize = NumInstances * 4 * sizeof(FVector4);

				if (!InstanceTransformBuffer.IsValid())
				{
					uint32 MaxBufferSize = MaxNumInstances * 4 * sizeof(FVector4);
					FRHIResourceCreateInfo CreateInfo;
					InstanceTransformBuffer.VertexBufferRHI = RHICreateVertexBuffer(MaxBufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					InstanceTransformBuffer.VertexBufferSRV = RHICreateShaderResourceView(InstanceTransformBuffer.VertexBufferRHI, sizeof(FVector4), PF_A32B32G32R32F);
				}

				if (InstanceTransformBuffer.IsValid())
				{
					FMatrix* LockedBuffer = (FMatrix*)RHILockVertexBuffer(InstanceTransformBuffer.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride

					for (uint32 i = 0; i < NumInstances; i++)
					{
						LockedBuffer[i] = InstanceData[i].Transform;
					}

					RHIUnlockVertexBuffer(InstanceTransformBuffer.VertexBufferRHI);
				}

				const uint32 InstanceDataStride = 8;
				BufferSize = NumInstances * sizeof(uint32) * InstanceDataStride;
				if (!InstanceAnimationBuffer.IsValid())
				{
					uint32 MaxBufferSize = MaxNumInstances * sizeof(uint32) * InstanceDataStride;
					FRHIResourceCreateInfo CreateInfo;
					InstanceAnimationBuffer.VertexBufferRHI = RHICreateVertexBuffer(MaxBufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					InstanceAnimationBuffer.VertexBufferSRV = RHICreateShaderResourceView(InstanceAnimationBuffer.VertexBufferRHI, sizeof(uint32), PF_R32_UINT);
				}

				if (InstanceAnimationBuffer.IsValid())
				{
					uint32* LockedBuffer = (uint32*)RHILockVertexBuffer(InstanceAnimationBuffer.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride

					uint32 NumBones = BoneData->GetNumBones();
					const TArray<uint32>& SequenceLength = BoneData->GetSequenceLength();
					const TArray<uint32>& SequenceOffset = BoneData->GetSequenceOffset();

					for (uint32 i = 0; i < NumInstances; i++)
					{
						uint32 Offset = i * InstanceDataStride;

						for (uint32 j = 0; j < 2; j++)
						{
							const auto& AnimData = InstanceData[i].AnimDatas[j];
							check(AnimData.Sequence >= 0 && AnimData.Sequence < SequenceLength.Num());
							const uint32 BufferOffest = SequenceOffset[AnimData.Sequence];
							LockedBuffer[Offset++] = BufferOffest + AnimData.PrevFrame * NumBones;
							LockedBuffer[Offset++] = BufferOffest + AnimData.NextFrame * NumBones;
							LockedBuffer[Offset++] = (uint32)(AnimData.FrameLerp * 1000);
							LockedBuffer[Offset++] = (uint32)(AnimData.BlendWeight * 1000);
						}
					}

					RHIUnlockVertexBuffer(InstanceAnimationBuffer.VertexBufferRHI);
				}

				return true;
			}
		private:
			const FSIAnimationData* BoneData = nullptr;
			FVertexBufferAndSRV BoneMap;
			FVertexBufferAndSRV RefBasesInvMatrix;
			FVertexBufferAndSRV InstanceTransformBuffer;
			FVertexBufferAndSRV InstanceAnimationBuffer;
		};

		const FShaderDataType& GetShaderData() const
		{
			return ShaderData;
		}

		FShaderDataType& GetShaderData()
		{
			return ShaderData;
		}

		static FVertexFactoryShaderParameters* ConstructShaderParameters(EShaderFrequency ShaderFrequency);
		static bool ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const FShaderType* ShaderType);
		static void ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment);

		void InitGPUSkinVertexFactoryComponents(const FVertexFactoryBuffers& VertexBuffers)
		{
			typedef TSkinWeightInfo<HasExtraBoneInfluences> WeightInfoType;

			//position
			VertexBuffers.StaticVertexBuffers->PositionVertexBuffer.BindPositionVertexBuffer(this, Data);

			// tangents
			VertexBuffers.StaticVertexBuffers->StaticMeshVertexBuffer.BindTangentVertexBuffer(this, Data);
			VertexBuffers.StaticVertexBuffers->StaticMeshVertexBuffer.BindTexCoordVertexBuffer(this, Data);

			// bone indices
			Data.BoneIndices = FVertexStreamComponent(
				VertexBuffers.SkinWeightVertexBuffer, STRUCT_OFFSET(WeightInfoType, InfluenceBones), VertexBuffers.SkinWeightVertexBuffer->GetStride(), VET_UByte4);
			// bone weights
			Data.BoneWeights = FVertexStreamComponent(
				VertexBuffers.SkinWeightVertexBuffer, STRUCT_OFFSET(WeightInfoType, InfluenceWeights), VertexBuffers.SkinWeightVertexBuffer->GetStride(), VET_UByte4N);

			Data.ColorComponentsSRV = nullptr;
			Data.ColorIndexMask = 0;
		}
	private:
		uint32 NumVertices;
		FDataType Data;
		FShaderDataType ShaderData;
	};

	void FGPUSkinVertexFactory::InitRHI()
	{
		// list of declaration items
		FVertexDeclarationElementList Elements;

		// position decls
		Elements.Add(AccessStreamComponent(Data.PositionComponent, 0));

		// tangent basis vector decls
		Elements.Add(AccessStreamComponent(Data.TangentBasisComponents[0], 1));
		Elements.Add(AccessStreamComponent(Data.TangentBasisComponents[1], 2));

		// texture coordinate decls
		if (Data.TextureCoordinates.Num())
		{
			const uint8 BaseTexCoordAttribute = 5;
			for (int32 CoordinateIndex = 0; CoordinateIndex < Data.TextureCoordinates.Num(); CoordinateIndex++)
			{
				Elements.Add(AccessStreamComponent(
					Data.TextureCoordinates[CoordinateIndex],
					BaseTexCoordAttribute + CoordinateIndex
				));
			}

			for (int32 CoordinateIndex = Data.TextureCoordinates.Num(); CoordinateIndex < MAX_TEXCOORDS; CoordinateIndex++)
			{
				Elements.Add(AccessStreamComponent(
					Data.TextureCoordinates[Data.TextureCoordinates.Num() - 1],
					BaseTexCoordAttribute + CoordinateIndex
				));
			}
		}

		if (Data.ColorComponentsSRV == nullptr)
		{
			Data.ColorComponentsSRV = GNullColorVertexBuffer.VertexBufferSRV;
			Data.ColorIndexMask = 0;
		}

		// Account for the possibility that the mesh has no vertex colors
		if (Data.ColorComponent.VertexBuffer)
		{
			Elements.Add(AccessStreamComponent(Data.ColorComponent, 13));
		}
		else
		{
			//If the mesh has no color component, set the null color buffer on a new stream with a stride of 0.
			//This wastes 4 bytes of bandwidth per vertex, but prevents having to compile out twice the number of vertex factories.
			FVertexStreamComponent NullColorComponent(&GNullColorVertexBuffer, 0, 0, VET_Color, EVertexStreamUsage::ManualFetch);
			Elements.Add(AccessStreamComponent(NullColorComponent, 13));
		}

		// bone indices decls
		Elements.Add(AccessStreamComponent(Data.BoneIndices, 3));

		// bone weights decls
		Elements.Add(AccessStreamComponent(Data.BoneWeights, 4));

		// create the actual device decls
		InitDeclaration(Elements);
	}

	void FGPUSkinVertexFactory::InitDynamicRHI()
	{
		FVertexFactory::InitDynamicRHI();
	}

	void FGPUSkinVertexFactory::ReleaseDynamicRHI()
	{
		FVertexFactory::ReleaseDynamicRHI();
		ShaderData.Release();
	}

	class FGPUSkinVertexFactoryShaderParameters : public FVertexFactoryShaderParameters
	{
	public:
		virtual void Bind(const FShaderParameterMap& ParameterMap) override
		{
			BoneMap.Bind(ParameterMap, TEXT("BoneMap"));
			RefBasesInvMatrix.Bind(ParameterMap, TEXT("RefBasesInvMatrix"));
			BoneMatrices.Bind(ParameterMap, TEXT("BoneMatrices"));
			InstanceMatrices.Bind(ParameterMap, TEXT("InstanceMatrices"));
			InstanceAnimations.Bind(ParameterMap, TEXT("InstanceAnimations"));
		}

		virtual void Serialize(FArchive& Ar) override
		{
			Ar << BoneMap;
			Ar << RefBasesInvMatrix;
			Ar << BoneMatrices;
			Ar << InstanceMatrices;
			Ar << InstanceAnimations;
		}

		virtual void GetElementShaderBindings(
			const FSceneInterface* Scene,
			const FSceneView* View,
			const FMeshMaterialShader* Shader,
			bool bShaderRequiresPositionOnlyStream,
			ERHIFeatureLevel::Type FeatureLevel,
			const FVertexFactory* VertexFactory,
			const FMeshBatchElement& BatchElement,
			class FMeshDrawSingleShaderBindings& ShaderBindings,
			FVertexInputStreamArray& VertexStreams) const override
		{
			const FGPUSkinVertexFactory::FShaderDataType& ShaderData = ((const FGPUSkinVertexFactory*)VertexFactory)->GetShaderData();

			if (BoneMap.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetBoneMapForReading().VertexBufferSRV;
				ShaderBindings.Add(BoneMap, CurrentData);
			}

			if (RefBasesInvMatrix.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetRefBasesInvMatrixForReading().VertexBufferSRV;
				ShaderBindings.Add(RefBasesInvMatrix, CurrentData);
			}

			if (BoneMatrices.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetBoneBufferForReading();
				ShaderBindings.Add(BoneMatrices, CurrentData);
			}

			if (InstanceMatrices.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetInstanceTransformBufferForReading().VertexBufferSRV;
				ShaderBindings.Add(InstanceMatrices, CurrentData);
			}

			if (InstanceAnimations.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetInstanceAnimationBufferForReading().VertexBufferSRV;
				ShaderBindings.Add(InstanceAnimations, CurrentData);
			}
		}

		virtual uint32 GetSize() const override { return sizeof(*this); }

	private:
		FShaderResourceParameter BoneMap;
		FShaderResourceParameter RefBasesInvMatrix;
		FShaderResourceParameter BoneMatrices;
		FShaderResourceParameter InstanceMatrices;
		FShaderResourceParameter InstanceAnimations;
	};

	FVertexFactoryShaderParameters* FGPUSkinVertexFactory::ConstructShaderParameters(EShaderFrequency ShaderFrequency)
	{
		return (ShaderFrequency == SF_Vertex) ? new FGPUSkinVertexFactoryShaderParameters() : NULL;
	}

	bool FGPUSkinVertexFactory::ShouldCompilePermutation(EShaderPlatform Platform, const class FMaterial* Material, const FShaderType* ShaderType)
	{
		return (Material->IsUsedWithSkeletalMesh() || Material->IsSpecialEngineMaterial());
	}

	void FGPUSkinVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryType* Type, EShaderPlatform Platform, const FMaterial* Material, FShaderCompilerEnvironment& OutEnvironment)
	{
		FVertexFactory::ModifyCompilationEnvironment(Type, Platform, Material, OutEnvironment);

		bool bLimit2BoneInfluences = (CVarSkinnedInstancingLimit2BoneInfluences.GetValueOnAnyThread() != 0);
		OutEnvironment.SetDefine(TEXT("SKINNED_INSTANCING_LIMIT_2BONE_INFLUENCES"), (bLimit2BoneInfluences ? 1 : 0));

		bool bDisableAnimationBlend = (CVarSkinnedInstancingDisableAnimationBlend.GetValueOnAnyThread() != 0);
		OutEnvironment.SetDefine(TEXT("SKINNED_INSTANCING_DISABLE_ANIMATION_BLEND"), (bDisableAnimationBlend ? 1 : 0));

		bool bDisableFrameLerp = (CVarSkinnedInstancingDisableFrameLerp.GetValueOnAnyThread() != 0);
		OutEnvironment.SetDefine(TEXT("SKINNED_INSTANCING_DISABLE_FRAME_LERP"), (bDisableFrameLerp ? 1 : 0));
	}
	
	FVertexFactoryType FGPUSkinVertexFactory::StaticType(
		TEXT("SkinnedInstancingVertexFactory"),
		TEXT("/Plugin/SkinnedInstancing/Private/SkinnedInstancingVertexFactory.ush"),
		/*bool bInUsedWithMaterials =*/ true,
		/*bool bInSupportsStaticLighting =*/ false,
		/*bool bInSupportsDynamicLighting =*/ true,
		/*bool bInSupportsPrecisePrevWorldPos =*/ false,
		/*bool bInSupportsPositionOnly =*/ false,
		/*bool bInSupportsCachingMeshDrawCommands =*/ false,
		/*bool bInSupportsPrimitiveIdStream =*/ false,
		FGPUSkinVertexFactory::ConstructShaderParameters,
		FGPUSkinVertexFactory::ShouldCompilePermutation,
		FGPUSkinVertexFactory::ModifyCompilationEnvironment,
		FGPUSkinVertexFactory::ValidateCompiledResult,
		FGPUSkinVertexFactory::SupportsTessellationShaders
	);

	FVertexFactoryType * FGPUSkinVertexFactory::GetType() const
	{
		return &StaticType;
	}
}

class FSIMeshObject : public FDeferredCleanupInterface
{
public:
	class FDynamicData
	{
	public:
		static FDynamicData* Alloc();
		static void Free(FDynamicData* Who);
		void Clear()
		{
			InstanceDatas.Reset();
		}
	public:
		TArray<FSIMeshInstanceData> InstanceDatas;
	};
public:
	FSIMeshObject(USkeletalMesh* SkeletalMesh, ERHIFeatureLevel::Type FeatureLevel);
	virtual ~FSIMeshObject();
public:
	virtual void ReleaseResources();
	FGPUSkinVertexFactory* GetSkinVertexFactory(int32 LODIndex, int32 ChunkIdx) const;
	void UpdateBoneData(const FSIAnimationData* AnimationData);
	const FDynamicData* GetDynamicData() const { return DynamicData; }
	void UpdateDynamicData(FDynamicData* NewDynamicData);
private:
	void UpdateBoneData(TArray<FMatrix>& BoneMatrices, int SequenceOffset,
		UAnimSequence* AnimSequence, const FBoneContainer* BoneContainer);
	void UpdateDynamicData_RenderThread(FDynamicData* NewDynamicData);
	void UpdateBoneData_RenderThread(const FSIAnimationData* AnimationData);
private:
	struct FSkeletalMeshObjectLOD;
private:
	ERHIFeatureLevel::Type FeatureLevel;
	class USkeletalMesh* SkeletalMesh;
	FSkeletalMeshRenderData* SkeletalMeshRenderData;
	TArray<FSkeletalMeshObjectLOD> LODs;
	FDynamicData* DynamicData;
public:
	TArray<TArray<FSIMeshInstanceData>> TempLODInstanceDatas;
};

struct FSIMeshObject::FSkeletalMeshObjectLOD
{
	void InitResources(FSkeletalMeshLODRenderData& LODData, ERHIFeatureLevel::Type InFeatureLevel)
	{
		// Vertex buffers available for the LOD
		FVertexFactoryBuffers VertexBuffers;

		VertexBuffers.StaticVertexBuffers = &LODData.StaticVertexBuffers;
		VertexBuffers.SkinWeightVertexBuffer = &LODData.SkinWeightVertexBuffer;
		VertexBuffers.NumVertices = LODData.GetNumVertices();

		// init gpu skin factories

		// first clear existing factories (resources assumed to have been released already)
		// then [re]create the factories

		VertexFactories.Empty(LODData.RenderSections.Num());

		for (int32 FactoryIdx = 0; FactoryIdx < LODData.RenderSections.Num(); ++FactoryIdx)
		{
			FGPUSkinVertexFactory* VertexFactory = new FGPUSkinVertexFactory(InFeatureLevel, VertexBuffers.NumVertices);
			VertexFactories.Add(TUniquePtr<FGPUSkinVertexFactory>(VertexFactory));

			// update vertex factory components and sync it
			ENQUEUE_RENDER_COMMAND(InitGPUSkinVertexFactory)(
				[VertexFactory, VertexBuffers](FRHICommandList& CmdList)
			{
				VertexFactory->InitGPUSkinVertexFactoryComponents(VertexBuffers);
			}
			);

			// init rendering resource	
			BeginInitResource(VertexFactory);
		}
	}

	void ReleaseResources()
	{
		// Release gpu skin vertex factories
		for (int32 FactoryIdx = 0; FactoryIdx < VertexFactories.Num(); FactoryIdx++)
		{
			BeginReleaseResource(VertexFactories[FactoryIdx].Get());
		}
	}

	SIZE_T GetResourceSize()
	{
		SIZE_T Size = 0;
		Size += VertexFactories.GetAllocatedSize();
		return Size;
	}

	// Per Section
	TArray<TUniquePtr<FGPUSkinVertexFactory>> VertexFactories;
};

namespace
{
	TArray<FSIMeshObject::FDynamicData*> FreeDynamicDatas;
	FCriticalSection FreeDynamicDatasCriticalSection;
	int32 GMinPoolCount = 0;
	int32 GAllocationCounter = 0;
	const int32 GAllocationsBeforeCleanup = 1000; // number of allocations we make before we clean up the pool, this number is increased when we have to allocate not from the pool
}

FSIMeshObject::FDynamicData * FSIMeshObject::FDynamicData::Alloc()
{
	FScopeLock S(&FreeDynamicDatasCriticalSection);
	++GAllocationCounter;
	GMinPoolCount = FMath::Min(FreeDynamicDatas.Num(), GMinPoolCount);
	if (FreeDynamicDatas.Num() > 0)
	{
		FDynamicData *Result = FreeDynamicDatas[0];
		FreeDynamicDatas.RemoveAtSwap(0);
		return Result;
	}
	else
	{
		return new FDynamicData;
	}
}

void FSIMeshObject::FDynamicData::Free(FDynamicData * Who)
{
	Who->Clear();
	FScopeLock S(&FreeDynamicDatasCriticalSection);
	FreeDynamicDatas.Add(Who);
	if (GAllocationCounter > GAllocationsBeforeCleanup)
	{
		GAllocationCounter = 0;
		for (int32 I = 0; I < GMinPoolCount; ++I)
		{
			delete FreeDynamicDatas[0];
			FreeDynamicDatas.RemoveAtSwap(0);
		}
		GMinPoolCount = FreeDynamicDatas.Num();
	}
}

FSIMeshObject::FSIMeshObject(USkeletalMesh* SkeletalMesh,
	ERHIFeatureLevel::Type FeatureLevel)
	: FeatureLevel(FeatureLevel)
	, SkeletalMesh(SkeletalMesh)
	, SkeletalMeshRenderData(SkeletalMesh->GetResourceForRendering())
	, DynamicData(nullptr)
{
	// create LODs to match the base mesh
	LODs.Empty(SkeletalMeshRenderData->LODRenderData.Num());

	for (int32 LODIndex = 0; LODIndex < SkeletalMeshRenderData->LODRenderData.Num(); LODIndex++)
	{
		new(LODs) FSkeletalMeshObjectLOD();

		// Skip LODs that have their render data stripped
		if (SkeletalMeshRenderData->LODRenderData.IsValidIndex(LODIndex)
			&& SkeletalMeshRenderData->LODRenderData[LODIndex].GetNumVertices() > 0)
		{
			LODs[LODIndex].InitResources(SkeletalMeshRenderData->LODRenderData[LODIndex],  FeatureLevel);
		}
	}
}

FSIMeshObject::~FSIMeshObject()
{
}

void FSIMeshObject::ReleaseResources()
{
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); LODIndex++)
	{
		LODs[LODIndex].ReleaseResources();
	}
}

FGPUSkinVertexFactory* FSIMeshObject::GetSkinVertexFactory(int32 LODIndex, int32 ChunkIdx) const
{
	checkSlow(LODs.IsValidIndex(LODIndex));

	const FSkeletalMeshObjectLOD& LOD = LODs[LODIndex];

	// use the default gpu skin vertex factory
	return LOD.VertexFactories[ChunkIdx].Get();
}

void FSIMeshObject::UpdateBoneData(TArray<FMatrix>& BoneMatrices, int SequenceOffset, UAnimSequence* AnimSequence, const FBoneContainer* BoneContainer)
{
	FCompactPose OutPose;
	FBlendedCurve OutCurve;
	OutPose.SetBoneContainer(BoneContainer);
	OutPose.ResetToRefPose();

	const FReferenceSkeleton& RefSkeleton = BoneContainer->GetReferenceSkeleton();

	int NumBones = RefSkeleton.GetRawBoneNum();
	int NumFrames = AnimSequence->GetNumberOfFrames();
	float Interval = (NumFrames > 1) ? (AnimSequence->SequenceLength / (NumFrames - 1)) : MINIMUM_ANIMATION_LENGTH;

	check(NumFrames > 0);

	for (int FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
	{
		float Time = FrameIndex * Interval;
		AnimSequence->GetBonePose(/*out*/ OutPose, /*out*/OutCurve, FAnimExtractContext(Time));

		TArray<FTransform> ComponentSpaceTransforms;

		ComponentSpaceTransforms.AddUninitialized(NumBones);

		auto& LocalTransform = OutPose.GetBones();

		check(LocalTransform.Num() == ComponentSpaceTransforms.Num());

		const FTransform* LocalTransformsData = LocalTransform.GetData();
		FTransform* ComponentSpaceData = ComponentSpaceTransforms.GetData();

		ComponentSpaceTransforms[0] = LocalTransform[0];

		for (int32 BoneIndex = 1; BoneIndex < LocalTransform.Num(); BoneIndex++)
		{
			// For all bones below the root, final component-space transform is relative transform * component-space transform of parent.
			const int32 ParentIndex = RefSkeleton.GetParentIndex(BoneIndex);
			FTransform* ParentSpaceBase = ComponentSpaceData + ParentIndex;
			FPlatformMisc::Prefetch(ParentSpaceBase);

			FTransform* SpaceBase = ComponentSpaceData + BoneIndex;

			FTransform::Multiply(SpaceBase, LocalTransformsData + BoneIndex, ParentSpaceBase);

			SpaceBase->NormalizeRotation();

			checkSlow(SpaceBase->IsRotationNormalized());
			checkSlow(!SpaceBase->ContainsNaN());
		}

		for (int BoneIndex = 0; BoneIndex < NumBones; BoneIndex++)
		{
			int PoseDataOffset = SequenceOffset + NumBones * FrameIndex + BoneIndex;
			BoneMatrices[PoseDataOffset] = ComponentSpaceTransforms[BoneIndex].ToMatrixWithScale();
		}
	}
}

void FSIMeshObject::UpdateBoneData(const FSIAnimationData* AnimationData)
{
	// queue a call to update this data
	ENQUEUE_RENDER_COMMAND(SIMeshObjectUpdateDataCommand)(
		[this, AnimationData](FRHICommandListImmediate& RHICmdList)
	{
		UpdateBoneData_RenderThread(AnimationData);
	}
	);
}

void FSIMeshObject::UpdateBoneData_RenderThread(const FSIAnimationData* AnimationData)
{
	for (int32 LODIndex = 0; LODIndex < SkeletalMeshRenderData->LODRenderData.Num(); LODIndex++)
	{
		const FSkeletalMeshLODRenderData& LODData = SkeletalMeshRenderData->LODRenderData[LODIndex];

		for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); SectionIndex++)
		{
			const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
			FGPUSkinVertexFactory* VertexFactory = GetSkinVertexFactory(LODIndex, SectionIndex);

			VertexFactory->GetShaderData().UpdateBoneData(AnimationData);

			TArray<FBoneIndexType> BoneMap;

			for (int BoneIndex = 0; BoneIndex < Section.BoneMap.Num(); BoneIndex++)
			{
				FName BoneName = SkeletalMesh->RefSkeleton.GetBoneName(Section.BoneMap[BoneIndex]);
				int NewBoneIndex = SkeletalMesh->Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName);
				BoneMap.Add(NewBoneIndex);
			}

			VertexFactory->GetShaderData().UpdateBoneMap(BoneMap);

			TArray<FMatrix> RefBasesInvMatrix;
			for (int BoneIndex = 0; BoneIndex < Section.BoneMap.Num(); BoneIndex++)
				RefBasesInvMatrix.Add(SkeletalMesh->RefBasesInvMatrix[Section.BoneMap[BoneIndex]]);

			VertexFactory->GetShaderData().UpdateRefBasesInvMatrix(RefBasesInvMatrix);
		}
	}
}

void FSIMeshObject::UpdateDynamicData(FDynamicData * NewDynamicData)
{
	// queue a call to update this data
	ENQUEUE_RENDER_COMMAND(SIMeshObjectUpdateDataCommand)(
		[this, NewDynamicData](FRHICommandListImmediate& RHICmdList)
	{
		UpdateDynamicData_RenderThread(NewDynamicData);
	}
	);
}

void FSIMeshObject::UpdateDynamicData_RenderThread(FDynamicData * NewDynamicData)
{
	if (DynamicData)
		FDynamicData::Free(DynamicData);
	DynamicData = NewDynamicData;
}

class FSIMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FSIMeshSceneProxy(USIMeshComponent* Component, 
		USkeletalMesh* SkeletalMesh, FSIMeshObject* MeshObject);
	virtual ~FSIMeshSceneProxy();

	
public: // override

	SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, 
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;

	virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) override;

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	virtual bool CanBeOccluded() const override
	{
		return !MaterialRelevance.bDisableDepthTest;
	}

	virtual uint32 GetMemoryFootprint(void) const override
	{
		return(sizeof(*this) + FPrimitiveSceneProxy::GetAllocatedSize());
	}

private:
	void GetDynamicMeshElementsByLOD(FMeshElementCollector & Collector, int32 ViewIndex, const FEngineShowFlags& EngineShowFlags,
		int LODIndex, const TArray< FSIMeshInstanceData>& InstanceData, int32 MaxNumInstances) const;

private:
	USIMeshComponent* Component;
	UBodySetup* BodySetup;
	FMaterialRelevance MaterialRelevance;
	TEnumAsByte<ERHIFeatureLevel::Type> FeatureLevel;
	class USkeletalMesh* SkeletalMesh;
	class FSIMeshObject* MeshObject;
	FSkeletalMeshRenderData* SkeletalMeshRenderData;
};

FSIMeshSceneProxy::FSIMeshSceneProxy(USIMeshComponent * Component,
	USkeletalMesh* SkeletalMesh, FSIMeshObject* MeshObject)
	: FPrimitiveSceneProxy(Component, Component->SkeletalMesh->GetFName())
	, Component(Component)
	, BodySetup(Component->GetBodySetup())
	, MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetFeatureLevel()))
	, FeatureLevel(GetScene().GetFeatureLevel())
	, SkeletalMesh(SkeletalMesh)
	, MeshObject(MeshObject)
	, SkeletalMeshRenderData(SkeletalMesh->GetResourceForRendering())
{
}

FSIMeshSceneProxy::~FSIMeshSceneProxy()
{
}

void FSIMeshSceneProxy::GetDynamicMeshElementsByLOD(FMeshElementCollector & Collector, int32 ViewIndex, const FEngineShowFlags& EngineShowFlags,
	int LODIndex, const TArray< FSIMeshInstanceData>& InstanceData, int32 MaxNumInstances) const
{
	const FSkeletalMeshLODRenderData& LODData = SkeletalMeshRenderData->LODRenderData[LODIndex];

	for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); SectionIndex++)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
		FGPUSkinVertexFactory* VertexFactory = MeshObject->GetSkinVertexFactory(LODIndex, SectionIndex);

		if (!VertexFactory)
			continue;

		// UpdateInstanceData
		VertexFactory->GetShaderData().UpdateInstanceData(InstanceData, MaxNumInstances);

		// Collect MeshBatch
		FMeshBatch& Mesh = Collector.AllocateMesh();

		Mesh.VertexFactory = MeshObject->GetSkinVertexFactory(LODIndex, SectionIndex);

		// Get material
		const FSkeletalMeshLODInfo& Info = *(SkeletalMesh->GetLODInfo(LODIndex));
		int32 UseMaterialIndex = Section.MaterialIndex;
		if (LODIndex > 0)
		{
			if (Section.MaterialIndex < Info.LODMaterialMap.Num())
			{
				UseMaterialIndex = Info.LODMaterialMap[Section.MaterialIndex];
				UseMaterialIndex = FMath::Clamp(UseMaterialIndex, 0, SkeletalMesh->Materials.Num());
			}
		}

		UMaterialInterface* Material = Component->GetMaterial(UseMaterialIndex);
		if (!Material)
		{
			Material = UMaterial::GetDefaultMaterial(MD_Surface);
		}

		if (Material)
		{
			Mesh.MaterialRenderProxy = Material->GetRenderProxy();
		}

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.FirstIndex = Section.BaseIndex;
		BatchElement.IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
		BatchElement.MaxVertexIndex = LODData.GetNumVertices() - 1;
		BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
		BatchElement.NumPrimitives = Section.NumTriangles;
		BatchElement.NumInstances = InstanceData.Num();

		Mesh.bWireframe |= EngineShowFlags.Wireframe;
		Mesh.Type = PT_TriangleList;
		Mesh.bSelectable = true;

		BatchElement.MinVertexIndex = Section.BaseVertexIndex;
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.CastShadow = true;
		Mesh.bCanApplyViewModeOverrides = true;

		if (ensureMsgf(Mesh.MaterialRenderProxy, TEXT("GetDynamicElementsSection with invalid MaterialRenderProxy. Owner:%s LODIndex:%d UseMaterialIndex:%d"), *GetOwnerName().ToString(), LODIndex, UseMaterialIndex) &&
			ensureMsgf(Mesh.MaterialRenderProxy->GetMaterial(FeatureLevel), TEXT("GetDynamicElementsSection with invalid FMaterial. Owner:%s LODIndex:%d UseMaterialIndex:%d"), *GetOwnerName().ToString(), LODIndex, UseMaterialIndex))
		{
			Collector.AddMesh(ViewIndex, Mesh);
		}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		RenderBounds(Collector.GetPDI(ViewIndex), EngineShowFlags, GetBounds(), IsSelected());
#endif
	}
}

int32 GetMinDesiredLODLevel(USkeletalMesh* SkeletalMesh, const FSceneView* View, const FVector4& Origin, const float SphereRadius)
{
	static const auto* SkeletalMeshLODRadiusScale = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.SkeletalMeshLODRadiusScale"));
	float LODScale = FMath::Clamp(SkeletalMeshLODRadiusScale->GetValueOnRenderThread(), 0.25f, 1.0f);

	const float ScreenRadiusSquared = ComputeBoundsScreenRadiusSquared(Origin, SphereRadius, *View) * LODScale * LODScale;

	// Need the current LOD
	const int32 CurrentLODLevel = 0;
	int32 NewLODLevel = 0;

	// Look for a lower LOD if the EngineShowFlags is enabled - Thumbnail rendering disables LODs
	if (View->Family && 1 == View->Family->EngineShowFlags.LOD)
	{
		int32 LODNum = SkeletalMesh->GetResourceForRendering()->LODRenderData.Num();

		// Iterate from worst to best LOD
		for (int32 LODLevel = LODNum - 1; LODLevel > 0; LODLevel--)
		{
			FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(LODLevel);

			// Get ScreenSize for this LOD
			float ScreenSize = LODInfo->ScreenSize.Default;

			// If we are considering shifting to a better (lower) LOD, bias with hysteresis.
			if (LODLevel <= CurrentLODLevel)
			{
				ScreenSize += LODInfo->LODHysteresis;
			}

			// If have passed this boundary, use this LOD
			if (FMath::Square(ScreenSize * 0.5f) > ScreenRadiusSquared)
			{
				NewLODLevel = LODLevel;
				break;
			}
		}
	}

	return NewLODLevel;
}

void FSIMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily & ViewFamily, uint32 VisibilityMap, FMeshElementCollector & Collector) const
{
	if (!MeshObject)
		return;

	auto DynamicData = MeshObject->GetDynamicData();
	if (!DynamicData || DynamicData->InstanceDatas.Num() <= 0)
		return;

	auto& LODInstanceDatas = MeshObject->TempLODInstanceDatas;

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			int32 MaxNumInstances = DynamicData->InstanceDatas.Num();
			int32 LODNum = SkeletalMeshRenderData->LODRenderData.Num();

			if (LODInstanceDatas.Num() < LODNum)
				LODInstanceDatas.AddDefaulted(LODNum - LODInstanceDatas.Num());

			for (int32 i = 0; i < LODNum; i++)
				LODInstanceDatas[i].Reset();

			// Calc LOD Instance
			for (auto Instance : DynamicData->InstanceDatas)
			{
				int LODLevel = GetMinDesiredLODLevel(SkeletalMesh, Views[ViewIndex], Instance.Transform.GetOrigin(), 100);
				check(LODLevel < LODNum);
				LODInstanceDatas[LODLevel].Add(Instance);
			}

			// Draw All LOD
			for (int32 LODIndex = 0; LODIndex < LODNum; LODIndex++)
			{
				if (LODInstanceDatas[LODIndex].Num() > 0)
				{
					GetDynamicMeshElementsByLOD(Collector, ViewIndex, ViewFamily.EngineShowFlags, LODIndex, LODInstanceDatas[LODIndex], MaxNumInstances);
				}
			}
		}
	}
}

FPrimitiveViewRelevance FSIMeshSceneProxy::GetViewRelevance(const FSceneView * View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	Result.bTranslucentSelfShadow = bCastVolumetricTranslucentShadow;
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	Result.bVelocityRelevance = IsMovable() && Result.bOpaqueRelevance && Result.bRenderInMainPass;
	return Result;
}

void FSIMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
}

//=========================================================
//		USIMeshComponent
//=========================================================

USIMeshComponent::USIMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;

	InstanceIdIncrease = 0;
}

FPrimitiveSceneProxy* USIMeshComponent::CreateSceneProxy()
{
	FSIMeshSceneProxy* Result = nullptr;
	FSkeletalMeshRenderData* SkelMeshRenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;

	// Only create a scene proxy for rendering if properly initialized
	if (SkelMeshRenderData)
	{
		Result = ::new FSIMeshSceneProxy(this, SkeletalMesh, MeshObject);
	}

	return Result;
}

int32 USIMeshComponent::GetNumMaterials() const
{
	if (SkeletalMesh)
	{
		return SkeletalMesh->Materials.Num();
	}

	return 0;
}

FBoxSphereBounds USIMeshComponent::CalcBounds(const FTransform & BoundTransform) const
{
	if (SkeletalMesh && PerInstanceSMData.Num() > 0)
	{
		FBoxSphereBounds RenderBounds = SkeletalMesh->GetBounds();
		FBoxSphereBounds NewBounds;

		bool IsFirst = true;
		for (auto& Elem : PerInstanceSMData)
		{
			if (IsFirst)
			{
				IsFirst = false;
				NewBounds = RenderBounds.TransformBy(Elem.Value.Transform);
			}
			else
			{
				NewBounds = NewBounds + RenderBounds.TransformBy(Elem.Value.Transform);
			}
		}

		return NewBounds;
	}
	else
	{
		return FBoxSphereBounds(BoundTransform.GetLocation(), FVector::ZeroVector, 0.f);
	}
}

void USIMeshComponent::OnRegister()
{
	Super::OnRegister();
}

void USIMeshComponent::OnUnregister()
{
	Super::OnUnregister();
}

void USIMeshComponent::CreateRenderState_Concurrent()
{
	if (SkeletalMesh && AnimationComponent.IsValid())
	{
		ERHIFeatureLevel::Type SceneFeatureLevel = GetWorld()->FeatureLevel;

		// Attempting to track down UE-45505, where it looks as if somehow a skeletal mesh component's mesh has only been partially loaded, causing a mismatch in the LOD arrays
		checkf(!SkeletalMesh->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WillBeLoaded), TEXT("Attempting to create render state for a skeletal mesh that is is not fully loaded. Mesh: %s"), *SkeletalMesh->GetName());

		// No need to create the mesh object if we aren't actually rendering anything (see UPrimitiveComponent::Attach)
		if (FApp::CanEverRender() && ShouldComponentAddToScene())
		{
			MeshObject = ::new FSIMeshObject(SkeletalMesh, SceneFeatureLevel);

			MeshObject->UpdateBoneData(AnimationComponent->GetAnimationData());
		}
	}

	UpdateMeshObejctDynamicData();

	Super::CreateRenderState_Concurrent();
}

void USIMeshComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();

	UpdateMeshObejctDynamicData();
}

void USIMeshComponent::DestroyRenderState_Concurrent()
{
	Super::DestroyRenderState_Concurrent();

	if (MeshObject)
	{
		// Begin releasing the RHI resources used by this skeletal mesh component.
		// This doesn't immediately destroy anything, since the rendering thread may still be using the resources.
		MeshObject->ReleaseResources();

		// Begin a deferred delete of MeshObject.  BeginCleanup will call MeshObject->FinishDestroy after the above release resource
		// commands execute in the rendering thread.
		BeginCleanup(MeshObject);
		MeshObject = nullptr;
	}
}

void USIMeshComponent::SetAnimationComponent(USIAnimationComponent * _AnimationComponent)
{
	AnimationComponent.Reset();
	AnimationComponent = _AnimationComponent;
	MarkRenderDynamicDataDirty();
}

void USIMeshComponent::UpdateMeshObejctDynamicData()
{
	if (MeshObject)
	{
		auto DynamicData = FSIMeshObject::FDynamicData::Alloc();
		DynamicData->InstanceDatas.Reserve(PerInstanceSMData.Num());
		for (auto Pair : PerInstanceSMData)
			DynamicData->InstanceDatas.Add(Pair.Value);
		MeshObject->UpdateDynamicData(DynamicData);
	}
}

int32 USIMeshComponent::AddInstance(const FTransform & Transform)
{
	int Id = ++InstanceIdIncrease;
	FSIMeshInstanceData NewInstanceData;

	NewInstanceData.Transform = Transform.ToMatrixWithScale();
	NewInstanceData.AnimDatas[0] = { 0, 0, 0, 0, 1 };
	NewInstanceData.AnimDatas[1] = { 0, 0, 0, 0, 0 };

	PerInstanceSMData.Add(Id, NewInstanceData);

	MarkRenderStateDirty();

	return Id;
}

void USIMeshComponent::RemoveInstance(int Id)
{
	PerInstanceSMData.Remove(Id);
	MarkRenderStateDirty();
}

UAnimSequence * USIMeshComponent::GetSequence(int Id)
{
	if (AnimationComponent.IsValid())
	{
		return AnimationComponent->GetSequence(Id);
	}

	return nullptr;
}

FSIMeshInstanceData* USIMeshComponent::GetInstanceData(int Id)
{
	return PerInstanceSMData.Find(Id);
}

void USIMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction * ThisTickFunction)
{
	// Tick ActorComponent first.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	UpdateBounds();
	MarkRenderTransformDirty();
	MarkRenderDynamicDataDirty();
}

#pragma optimize( "", on )
