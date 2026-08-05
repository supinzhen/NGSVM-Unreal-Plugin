// Fill out your copyri// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"

#include "NGSVMManager.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "GameFramework/Controller.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/FloatingPawnMovement.h"

#include "NGSVMBillboardPawn.generated.h"

UCLASS()
class NGSVMCORE_API ANGSVMBillboardPawn : public APawn
{
	GENERATED_BODY()

public:
	// Sets default values for this pawn's properties
	ANGSVMBillboardPawn();

protected:
	// Called when the game starts or when spawned
	virtual void BeginPlay() override;

public:	
	// Called every frame
	virtual void Tick(float DeltaTime) override;

	// Called to bind functionality to input
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	TObjectPtr<USceneComponent> RootScene;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	TObjectPtr<UStaticMeshComponent> BillboardMesh;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	USpringArmComponent* CameraBoom;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	UCameraComponent* FollowCamera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	UFloatingPawnMovement* MovementComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "NGSVM|Components")
	UNGSVMManager* NgsvmManager;


	void CameraMove(const FInputActionValue& Value);

	void CameraLook(const FInputActionValue& Value);

	void CameraUpDown(const FInputActionValue& Value);

	void CameraZoom(const FInputActionValue& Value);

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NGSVM|EnhancedInput")
	UInputMappingContext* InputMapping;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NGSVM|EnhancedInput")
	UInputAction* MoveAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NGSVM|EnhancedInput")
	UInputAction* LookAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NGSVM|EnhancedInput")
	UInputAction* UpDownAction;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NGSVM|EnhancedInput")
	UInputAction* ZoomAction;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Movement")
	float MoveSpeedScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Movement")
	float ZoomSpeedScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Billboard")
	bool bFaceCamera = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Billboard")
	FRotator MeshOrientationCorrection = FRotator(90.0f, 0.0f, 90.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Camera")
	float DefaultSpringArm = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Camera")
	float MaxSpringArm = 500.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NGSVM|Camera")
	float MinSpringArm = 100.0f;

	void UpdateBillboardFacing();

	void UpdateBillboardScale();
};
