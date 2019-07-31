#include "Components/InstancedSkinnedMeshComponent.h"
#include "PrimitiveSceneProxy.h"
#include "SkeletalMeshTypes.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Animation/AnimSequence.h"
#include "BonePose.h"
#include "MeshDrawShaderBindings.h"

#pragma optimize( "", off )
namespace
{
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
				BoneBuffer.SafeRelease();
				InstanceTransformBuffer.SafeRelease();
				InstanceAnimationBuffer.SafeRelease();
			}

			const FVertexBufferAndSRV& GetBoneBufferForReading() const
			{
				return BoneBuffer;
			}

			const FVertexBufferAndSRV& GetInstanceTransformBufferForReading() const
			{
				return InstanceTransformBuffer;
			}

			const FVertexBufferAndSRV& GetInstanceAnimationBufferForReading() const
			{
				return InstanceAnimationBuffer;
			}

			bool UpdateBoneData(const TArray<FMatrix>& ReferenceToLocalMatrices)
			{
				uint32 BufferSize = ReferenceToLocalMatrices.Num() * 3 * sizeof(FVector4);

				if (!BoneBuffer.IsValid())
				{
					FRHIResourceCreateInfo CreateInfo;
					BoneBuffer.VertexBufferRHI = RHICreateVertexBuffer(BufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					BoneBuffer.VertexBufferSRV = RHICreateShaderResourceView(BoneBuffer.VertexBufferRHI, sizeof(FVector4), PF_A32B32G32R32F);
				}
				
				if (BoneBuffer.IsValid() && ReferenceToLocalMatrices.Num() > 0)
				{
					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
					FMatrix3x4* LockedBuffer = (FMatrix3x4*)RHILockVertexBuffer(BoneBuffer.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					for (size_t i = 0; i < ReferenceToLocalMatrices.Num(); i++)
					{
						FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + i + PreFetchStride);
						FPlatformMisc::Prefetch(ReferenceToLocalMatrices.GetData() + i + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

						FMatrix3x4& BoneMat = LockedBuffer[i];
						const FMatrix& RefToLocal = ReferenceToLocalMatrices[i];
						RefToLocal.To3x4MatrixTranspose((float*)BoneMat.M);
					}

					RHIUnlockVertexBuffer(BoneBuffer.VertexBufferRHI);
				}

				return true;
			}

			bool UpdateInstanceData(const TArray<FInstancedSkinnedMeshInstanceData>& InstanceTransforms, int NumBones)
			{
				const uint32 NumInstances = InstanceTransforms.Num();
				uint32 BufferSize = NumInstances * 4 * sizeof(FVector4);

				if (!InstanceTransformBuffer.IsValid())
				{
					FRHIResourceCreateInfo CreateInfo;
					InstanceTransformBuffer.VertexBufferRHI = RHICreateVertexBuffer(BufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					InstanceTransformBuffer.VertexBufferSRV = RHICreateShaderResourceView(InstanceTransformBuffer.VertexBufferRHI, sizeof(FVector4), PF_A32B32G32R32F);
				}

				if (InstanceTransformBuffer.IsValid())
				{
					FMatrix* LockedBuffer = (FMatrix*)RHILockVertexBuffer(InstanceTransformBuffer.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
					for (uint32 i = 0; i < NumInstances; i++)
					{
						FPlatformMisc::Prefetch(InstanceTransforms.GetData() + i + PreFetchStride);
						FPlatformMisc::Prefetch(InstanceTransforms.GetData() + i + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

						LockedBuffer[i] = InstanceTransforms[i].Transform;
					}

					RHIUnlockVertexBuffer(InstanceTransformBuffer.VertexBufferRHI);
				}

				const int InstanceDataCount = 3;
				BufferSize = NumInstances * sizeof(UINT) * InstanceDataCount;
				if (!InstanceAnimationBuffer.IsValid())
				{
					FRHIResourceCreateInfo CreateInfo;
					InstanceAnimationBuffer.VertexBufferRHI = RHICreateVertexBuffer(BufferSize, (BUF_Dynamic | BUF_ShaderResource), CreateInfo);
					InstanceAnimationBuffer.VertexBufferSRV = RHICreateShaderResourceView(InstanceAnimationBuffer.VertexBufferRHI, sizeof(UINT), PF_R32_UINT);
				}

				if (InstanceAnimationBuffer.IsValid())
				{
					UINT* LockedBuffer = (UINT*)RHILockVertexBuffer(InstanceAnimationBuffer.VertexBufferRHI, 0, BufferSize, RLM_WriteOnly);

					const int32 PreFetchStride = 2; // FPlatformMisc::Prefetch stride
					for (uint32 i = 0; i < NumInstances; i++)
					{
						FPlatformMisc::Prefetch(InstanceTransforms.GetData() + i + PreFetchStride);
						FPlatformMisc::Prefetch(InstanceTransforms.GetData() + i + PreFetchStride, PLATFORM_CACHE_LINE_SIZE);

						LockedBuffer[i * InstanceDataCount + 0] = InstanceTransforms[i].PrevFrame * NumBones;
						LockedBuffer[i * InstanceDataCount + 1] = InstanceTransforms[i].NextFrame * NumBones;
						LockedBuffer[i * InstanceDataCount + 2] = InstanceTransforms[i].FrameLerp;
					}

					RHIUnlockVertexBuffer(InstanceAnimationBuffer.VertexBufferRHI);
				}

				return true;
			}
		private:
			FVertexBufferAndSRV BoneBuffer;
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
			BoneMatrices.Bind(ParameterMap, TEXT("BoneMatrices"));
			InstanceMatrices.Bind(ParameterMap, TEXT("InstanceMatrices"));
			InstanceAnimations.Bind(ParameterMap, TEXT("InstanceAnimations"));
		}

		virtual void Serialize(FArchive& Ar) override
		{
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

			if (BoneMatrices.IsBound())
			{
				FShaderResourceViewRHIParamRef CurrentData = ShaderData.GetBoneBufferForReading().VertexBufferSRV;
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
	}
	
	FVertexFactoryType FGPUSkinVertexFactory::StaticType(
		TEXT("InstancedSkinVertexFactory"),
		TEXT("/Engine/Private/InstancedSkinVertexFactory.ush"),
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

class FInstancedSkinnedMeshObject : public FDeferredCleanupInterface
{
public:
	FInstancedSkinnedMeshObject(USkeletalMesh* SkeletalMesh, ERHIFeatureLevel::Type FeatureLevel, UAnimSequence* AnimSequence);
	virtual ~FInstancedSkinnedMeshObject();
public:
	virtual void InitResources();
	virtual void ReleaseResources();
	FGPUSkinVertexFactory* GetSkinVertexFactory(int32 LODIndex, int32 ChunkIdx) const;
	void UpdateBoneDataDeferred();
private:
	class FVertexFactoryData;
	struct FSkeletalMeshObjectLOD;
private:
	ERHIFeatureLevel::Type FeatureLevel;
	class USkeletalMesh* SkeletalMesh;
	FSkeletalMeshRenderData* SkeletalMeshRenderData;
	TArray<FSkeletalMeshObjectLOD> LODs;
	bool bBoneDataUpdated;
	UAnimSequence* AnimSequence;
};

class FInstancedSkinnedMeshObject::FVertexFactoryData
{
public:
	FVertexFactoryData() {}

	TArray<TUniquePtr<FGPUSkinVertexFactory>> VertexFactories;

	void InitVertexFactories(
		const FVertexFactoryBuffers& InVertexBuffers,
		const TArray<FSkelMeshRenderSection>& Sections,
		ERHIFeatureLevel::Type InFeatureLevel)
	{
		// first clear existing factories (resources assumed to have been released already)
		// then [re]create the factories

		VertexFactories.Empty(Sections.Num());
		{
			for (int32 FactoryIdx = 0; FactoryIdx < Sections.Num(); ++FactoryIdx)
			{
				FGPUSkinVertexFactory* VertexFactory = new FGPUSkinVertexFactory(InFeatureLevel, InVertexBuffers.NumVertices);
				VertexFactories.Add(TUniquePtr<FGPUSkinVertexFactory>(VertexFactory));

				// update vertex factory components and sync it
				ENQUEUE_RENDER_COMMAND(InitGPUSkinVertexFactory)(
					[VertexFactory, InVertexBuffers](FRHICommandList& CmdList)
				{
					VertexFactory->InitGPUSkinVertexFactoryComponents(InVertexBuffers);
				}
				);

				// init rendering resource	
				BeginInitResource(VertexFactory);
			}
		}
	}

	void ReleaseVertexFactories()
	{
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
};

struct FInstancedSkinnedMeshObject::FSkeletalMeshObjectLOD
{
	FSkeletalMeshObjectLOD(FSkeletalMeshRenderData* InSkelMeshRenderData, int32 InLOD)
		: SkelMeshRenderData(InSkelMeshRenderData)
		, LODIndex(InLOD)
	{
	}

	void InitResources(ERHIFeatureLevel::Type InFeatureLevel)
	{
		check(SkelMeshRenderData);
		check(SkelMeshRenderData->LODRenderData.IsValidIndex(LODIndex));

		// Vertex buffers available for the LOD
		FVertexFactoryBuffers VertexBuffers;

		// vertex buffer for each lod has already been created when skelmesh was loaded
		FSkeletalMeshLODRenderData& LODData = SkelMeshRenderData->LODRenderData[LODIndex];

		VertexBuffers.StaticVertexBuffers = &LODData.StaticVertexBuffers;
		VertexBuffers.SkinWeightVertexBuffer = &LODData.SkinWeightVertexBuffer;
		VertexBuffers.NumVertices = LODData.GetNumVertices();

		// init gpu skin factories
		GPUSkinVertexFactories.InitVertexFactories(VertexBuffers, LODData.RenderSections, InFeatureLevel);
	}

	void ReleaseResources()
	{
		// Release gpu skin vertex factories
		GPUSkinVertexFactories.ReleaseVertexFactories();
	}

public:
	FSkeletalMeshRenderData* SkelMeshRenderData;

	// index into FSkeletalMeshRenderData::LODRenderData[]
	int32 LODIndex;

	/** Default GPU skinning vertex factories and matrices */
	FVertexFactoryData GPUSkinVertexFactories;
};

FInstancedSkinnedMeshObject::FInstancedSkinnedMeshObject(USkeletalMesh* SkeletalMesh,
	ERHIFeatureLevel::Type FeatureLevel,
	UAnimSequence* AnimSequence)
	: SkeletalMesh(SkeletalMesh)
	, SkeletalMeshRenderData(SkeletalMesh->GetResourceForRendering())
	, FeatureLevel(FeatureLevel)
	, bBoneDataUpdated(false)
	, AnimSequence(AnimSequence)
{
	// create LODs to match the base mesh
	LODs.Empty(SkeletalMeshRenderData->LODRenderData.Num());
	for (int32 LODIndex = 0; LODIndex < SkeletalMeshRenderData->LODRenderData.Num(); LODIndex++)
	{
		new(LODs) FSkeletalMeshObjectLOD(SkeletalMeshRenderData, LODIndex);
	}

	InitResources();
}

FInstancedSkinnedMeshObject::~FInstancedSkinnedMeshObject()
{
}

void FInstancedSkinnedMeshObject::InitResources()
{
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); LODIndex++)
	{
		FSkeletalMeshObjectLOD& SkelLOD = LODs[LODIndex];

		// Skip LODs that have their render data stripped
		if (SkelLOD.SkelMeshRenderData
			&& SkelLOD.SkelMeshRenderData->LODRenderData.IsValidIndex(LODIndex)
			&& SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].GetNumVertices() > 0)
		{
			SkelLOD.InitResources(FeatureLevel);
		}
	}
}

void FInstancedSkinnedMeshObject::ReleaseResources()
{
	for (int32 LODIndex = 0; LODIndex < LODs.Num(); LODIndex++)
	{
		FSkeletalMeshObjectLOD& SkelLOD = LODs[LODIndex];

		// Skip LODs that have their render data stripped
		if (SkelLOD.SkelMeshRenderData 
			&& SkelLOD.SkelMeshRenderData->LODRenderData.IsValidIndex(LODIndex) 
			&& SkelLOD.SkelMeshRenderData->LODRenderData[LODIndex].GetNumVertices() > 0)
		{
			SkelLOD.ReleaseResources();
		}
	}
}

FGPUSkinVertexFactory* FInstancedSkinnedMeshObject::GetSkinVertexFactory(int32 LODIndex, int32 ChunkIdx) const
{
	checkSlow(LODs.IsValidIndex(LODIndex));

	const FSkeletalMeshObjectLOD& LOD = LODs[LODIndex];

	// use the default gpu skin vertex factory
	return LOD.GPUSkinVertexFactories.VertexFactories[ChunkIdx].Get();
}

void FInstancedSkinnedMeshObject::UpdateBoneDataDeferred()
{
	if (bBoneDataUpdated)
		return;

	bBoneDataUpdated = true;

	if (!AnimSequence)
		return;

	int NumFrames = AnimSequence->GetNumberOfFrames();
	float Interval = (NumFrames > 1) ? (AnimSequence->SequenceLength / (NumFrames - 1)) : MINIMUM_ANIMATION_LENGTH;

	check(NumFrames > 0);

	const int32 LODIndex = 0;
	check(LODIndex < SkeletalMeshRenderData->LODRenderData.Num());

	const FSkeletalMeshLODRenderData& LODData = SkeletalMeshRenderData->LODRenderData[LODIndex];

	FBoneContainer BoneContainer;
	const TArray<FBoneIndexType>& RequiredBones = LODData.RequiredBones;
	BoneContainer.InitializeTo(RequiredBones, FCurveEvaluationOption(), *SkeletalMesh);

	FCompactPose OutPose;
	FBlendedCurve OutCurve;
	OutPose.SetBoneContainer(&BoneContainer);
	OutPose.ResetToRefPose();

	for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); SectionIndex++)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
		FGPUSkinVertexFactory* VertexFactory = GetSkinVertexFactory(LODIndex, SectionIndex);
		const TArray<FBoneIndexType>& BoneMap = Section.BoneMap;

		TArray<FMatrix> PoseData;
		PoseData.AddUninitialized(BoneMap.Num() * NumFrames);

		for (int FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++)
		{
			float Time = FrameIndex * Interval;
			AnimSequence->GetBonePose(/*out*/ OutPose, /*out*/OutCurve, FAnimExtractContext(Time));

			int NumBones = SkeletalMesh->RefSkeleton.GetNum();
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
				const int32 ParentIndex = SkeletalMesh->RefSkeleton.GetParentIndex(BoneIndex);
				FTransform* ParentSpaceBase = ComponentSpaceData + ParentIndex;
				FPlatformMisc::Prefetch(ParentSpaceBase);

				FTransform* SpaceBase = ComponentSpaceData + BoneIndex;

				FTransform::Multiply(SpaceBase, LocalTransformsData + BoneIndex, ParentSpaceBase);

				SpaceBase->NormalizeRotation();

				checkSlow(SpaceBase->IsRotationNormalized());
				checkSlow(!SpaceBase->ContainsNaN());
			}

			for (int BoneIndex = 0; BoneIndex < BoneMap.Num(); BoneIndex++)
			{
				const FBoneIndexType RefIndex = BoneMap[BoneIndex]; // Indices of bones in the USkeletalMesh::RefSkeleton array
				int PoseDataOffset = BoneMap.Num() * FrameIndex + BoneIndex;
				PoseData[PoseDataOffset] = SkeletalMesh->RefBasesInvMatrix[RefIndex] * ComponentSpaceTransforms[RefIndex].ToMatrixWithScale();
			}
		}

		VertexFactory->GetShaderData().UpdateBoneData(PoseData);
	}
}

class FInstancedSkinnedMeshSceneProxy final : public FPrimitiveSceneProxy
{
public:
	FInstancedSkinnedMeshSceneProxy(UInstancedSkinnedMeshComponent* Component, 
		USkeletalMesh* SkeletalMesh, FInstancedSkinnedMeshObject* MeshObject);
	virtual ~FInstancedSkinnedMeshSceneProxy();

	
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
	UInstancedSkinnedMeshComponent* Component;
	UBodySetup* BodySetup;
	FMaterialRelevance MaterialRelevance;
	TEnumAsByte<ERHIFeatureLevel::Type> FeatureLevel;
	class USkeletalMesh* SkeletalMesh;
	class FInstancedSkinnedMeshObject* MeshObject;
	FSkeletalMeshRenderData* SkeletalMeshRenderData;
};

FInstancedSkinnedMeshSceneProxy::FInstancedSkinnedMeshSceneProxy(UInstancedSkinnedMeshComponent * Component,
	USkeletalMesh* SkeletalMesh, FInstancedSkinnedMeshObject* MeshObject)
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

FInstancedSkinnedMeshSceneProxy::~FInstancedSkinnedMeshSceneProxy()
{
}

void FInstancedSkinnedMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily & ViewFamily, uint32 VisibilityMap, FMeshElementCollector & Collector) const
{
	if (Component->PerInstanceSMData.Num() <= 0)
		return;

	MeshObject->UpdateBoneDataDeferred();

	const int32 LODIndex = 0;
	check(LODIndex < SkeletalMeshRenderData->LODRenderData.Num());

	const FSkeletalMeshLODRenderData& LODData = SkeletalMeshRenderData->LODRenderData[LODIndex];

	for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); SectionIndex++)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
		FGPUSkinVertexFactory* VertexFactory = MeshObject->GetSkinVertexFactory(LODIndex, SectionIndex);
		VertexFactory->GetShaderData().UpdateInstanceData(Component->PerInstanceSMData, Section.BoneMap.Num());
	}

	for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); SectionIndex++)
	{
		const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];

		const bool bIsWireframe = ViewFamily.EngineShowFlags.Wireframe;

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (VisibilityMap & (1 << ViewIndex))
			{
				const FSceneView* View = Views[ViewIndex];

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
				BatchElement.FirstIndex = LODData.RenderSections[SectionIndex].BaseIndex;
				BatchElement.IndexBuffer = LODData.MultiSizeIndexContainer.GetIndexBuffer();
				BatchElement.MaxVertexIndex = LODData.GetNumVertices() - 1;
				BatchElement.PrimitiveUniformBuffer = GetUniformBuffer();
				BatchElement.NumPrimitives = LODData.RenderSections[SectionIndex].NumTriangles;
				BatchElement.NumInstances = Component->PerInstanceSMData.Num();

				if (!Mesh.VertexFactory)
				{
					// hide this part
					continue;
				}

				Mesh.bWireframe |= bIsWireframe;
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
			}
		}
	}
}

FPrimitiveViewRelevance FInstancedSkinnedMeshSceneProxy::GetViewRelevance(const FSceneView * View) const
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

void FInstancedSkinnedMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
}

//=========================================================
//		UInstancedSkinnedMeshComponent
//=========================================================

UInstancedSkinnedMeshComponent::UInstancedSkinnedMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bAutoActivate = true;
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
}

FPrimitiveSceneProxy* UInstancedSkinnedMeshComponent::CreateSceneProxy()
{
	FInstancedSkinnedMeshSceneProxy* Result = nullptr;
	FSkeletalMeshRenderData* SkelMeshRenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;

	// Only create a scene proxy for rendering if properly initialized
	if (SkelMeshRenderData)
	{
		Result = ::new FInstancedSkinnedMeshSceneProxy(this, SkeletalMesh, MeshObject);
	}

	return Result;
}

int32 UInstancedSkinnedMeshComponent::GetNumMaterials() const
{
	if (SkeletalMesh)
	{
		return SkeletalMesh->Materials.Num();
	}

	return 0;
}

FBoxSphereBounds UInstancedSkinnedMeshComponent::CalcBounds(const FTransform & BoundTransform) const
{
	if (SkeletalMesh && PerInstanceSMData.Num() > 0)
	{
		FMatrix BoundTransformMatrix = BoundTransform.ToMatrixWithScale();

		FBoxSphereBounds RenderBounds = SkeletalMesh->GetBounds();
		FBoxSphereBounds NewBounds = RenderBounds.TransformBy(PerInstanceSMData[0].Transform * BoundTransformMatrix);

		for (int32 InstanceIndex = 1; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
		{
			NewBounds = NewBounds + RenderBounds.TransformBy(PerInstanceSMData[InstanceIndex].Transform * BoundTransformMatrix);
		}

		return NewBounds;
	}
	else
	{
		return FBoxSphereBounds(BoundTransform.GetLocation(), FVector::ZeroVector, 0.f);
	}
}

void UInstancedSkinnedMeshComponent::OnRegister()
{
	Super::OnRegister();
}

void UInstancedSkinnedMeshComponent::OnUnregister()
{
	Super::OnUnregister();
}

void UInstancedSkinnedMeshComponent::CreateRenderState_Concurrent()
{
	if (SkeletalMesh && AnimSequence)
	{
		ERHIFeatureLevel::Type SceneFeatureLevel = GetWorld()->FeatureLevel;

		// Attempting to track down UE-45505, where it looks as if somehow a skeletal mesh component's mesh has only been partially loaded, causing a mismatch in the LOD arrays
		checkf(!SkeletalMesh->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad | RF_NeedPostLoadSubobjects | RF_WillBeLoaded), TEXT("Attempting to create render state for a skeletal mesh that is is not fully loaded. Mesh: %s"), *SkeletalMesh->GetName());

		// No need to create the mesh object if we aren't actually rendering anything (see UPrimitiveComponent::Attach)
		if (FApp::CanEverRender() && ShouldComponentAddToScene())
		{
			MeshObject = ::new FInstancedSkinnedMeshObject(SkeletalMesh, SceneFeatureLevel, AnimSequence);
		}
	}

	Super::CreateRenderState_Concurrent();
}

void UInstancedSkinnedMeshComponent::SendRenderDynamicData_Concurrent()
{
	Super::SendRenderDynamicData_Concurrent();
}

void UInstancedSkinnedMeshComponent::DestroyRenderState_Concurrent()
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

int32 UInstancedSkinnedMeshComponent::AddInstanceInternal(int32 InstanceIndex, FInstancedSkinnedMeshInstanceData * InNewInstanceData, 
	const FTransform & Transform)
{
	FInstancedSkinnedMeshInstanceData* NewInstanceData = InNewInstanceData;

	if (NewInstanceData == nullptr)
	{
		NewInstanceData = new(PerInstanceSMData) FInstancedSkinnedMeshInstanceData();
	}

	NewInstanceData->Transform = Transform.ToMatrixWithScale();
	NewInstanceData->Time = FMath::RandRange(0.0f, AnimSequence->SequenceLength);

	MarkRenderStateDirty();

	return InstanceIndex;
}

int32 UInstancedSkinnedMeshComponent::AddInstance(const FTransform & Transform)
{
	return AddInstanceInternal(PerInstanceSMData.Num(), nullptr, Transform);
}

void UInstancedSkinnedMeshComponent::SetInstance(int32 Index, const FTransform & Transform, float Time)
{
	check(Index < PerInstanceSMData.Num());
	PerInstanceSMData[Index].Transform = Transform.ToMatrixWithScale();;
	PerInstanceSMData[Index].Time = Time;
}

void UInstancedSkinnedMeshComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction * ThisTickFunction)
{
	// Tick ActorComponent first.
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (AnimSequence && SkeletalMesh)
	{
		int NumFrames = AnimSequence->GetNumberOfFrames();
		float SequenceLength = AnimSequence->SequenceLength;
		float Interval = (NumFrames > 1) ? (SequenceLength / (NumFrames - 1)) : MINIMUM_ANIMATION_LENGTH;

		for (int InstanceIndex = 0; InstanceIndex < PerInstanceSMData.Num(); InstanceIndex++)
		{
			auto& Instance = PerInstanceSMData[InstanceIndex];

			// Test Start
			Instance.Time += DeltaTime;
			// Test End

			float Time = FMath::Fmod(Instance.Time, SequenceLength);
			int Frame = Time / Interval;
			float Lerp = Time - Frame * Interval;

			Instance.PrevFrame = FMath::Clamp(Frame, 0, NumFrames - 1);
			Instance.NextFrame = FMath::Clamp(Frame + 1, 0, NumFrames - 1);
			Instance.FrameLerp = FMath::Clamp(Lerp, 0.0f, 1.0f) * 1000;
		}
	}
}

#pragma optimize( "", on )
