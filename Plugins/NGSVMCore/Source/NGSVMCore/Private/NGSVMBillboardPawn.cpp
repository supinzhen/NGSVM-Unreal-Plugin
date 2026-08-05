// Copyright (c) 2026 Su, Pin-Chen (Annie Su) / PZN Studio. Licensed under the terms in LICENSE.


#include "NGSVMBillboardPawn.h"
#include "Components/StaticMeshComponent.h"
#include "NGSVMManager.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"
// Full definitions required (not just the forward declarations pulled in transitively via
// PlayerController.h/GenericPlatformMisc.h) -- ULocalPlayer::GetSubsystem<>() and GetWorld()'s
// return value both need complete types here. Relying on some other unity-build-neighboring file
// to have already included these is fragile; this broke as soon as the Game (non-Editor) target's
// unity grouping put this file next to different neighbors than the Editor target does.
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"

// Sets default values
ANGSVMBillboardPawn::ANGSVMBillboardPawn()
{
 	// Set this pawn to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = true;


	RootScene = CreateDefaultSubobject<USceneComponent>(TEXT("RootScene"));
	RootComponent = RootScene;

	BillboardMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("BillboardMesh"));
	BillboardMesh->SetupAttachment(RootComponent);
	BillboardMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = DefaultSpringArm;
	CameraBoom->bUsePawnControlRotation = true; // Rotate the arm based on the controller

	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	// Attach the camera to the end of the boom and let the boom adjust to match the controller orientation
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->bUsePawnControlRotation = false; // Camera does not rotate relative to arm

	// 3. �L���O�������ʤ���
	MovementComponent = CreateDefaultSubobject<UFloatingPawnMovement>(TEXT("MovementComponent"));
	MovementComponent->UpdatedComponent = RootComponent;
	MovementComponent->MaxSpeed = 100.0f;
	MovementComponent->Acceleration = 2000.0f;
	MovementComponent->Deceleration = 2000.0f;

	NgsvmManager = CreateDefaultSubobject<UNGSVMManager>(TEXT("NGSVMManager"));


}

// Called when the game starts or when spawned
void ANGSVMBillboardPawn::BeginPlay()
{
	Super::BeginPlay();

	UpdateBillboardScale();
	
}

// Called every frame
void ANGSVMBillboardPawn::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdateBillboardFacing();

}

// Called to bind functionality to input
void ANGSVMBillboardPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (APlayerController* PlayerController = Cast<APlayerController>(Controller))
	{
		// Get Local Player subsystem
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			// Add input context
			Subsystem->AddMappingContext(InputMapping, 0);
		}
	}

	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ANGSVMBillboardPawn::CameraMove);
		Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ANGSVMBillboardPawn::CameraLook);
		Input->BindAction(UpDownAction, ETriggerEvent::Triggered, this, &ANGSVMBillboardPawn::CameraUpDown);
		Input->BindAction(ZoomAction, ETriggerEvent::Triggered, this, &ANGSVMBillboardPawn::CameraZoom);
	}

}

void ANGSVMBillboardPawn::CameraMove(const FInputActionValue& Value)
{
	FVector2D InputValue = Value.Get<FVector2D>();

	if (IsValid(Controller))
	{
		// Get Forward Direction
		const FRotator Rotation = Controller->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// Add movement input
		AddMovementInput(ForwardDirection, InputValue.Y);
		AddMovementInput(RightDirection, InputValue.X);
	}
}


void ANGSVMBillboardPawn::CameraLook(const FInputActionValue& Value)
{
	FVector2D InputValue = Value.Get<FVector2D>();

	if (IsValid(Controller))
	{
		AddControllerYawInput(InputValue.X);
		AddControllerPitchInput(InputValue.Y);
	}
}

void ANGSVMBillboardPawn::CameraUpDown(const FInputActionValue& Value)
{
	float UpValue = Value.Get<float>();
	AddMovementInput(FVector::UpVector, UpValue);
}

void ANGSVMBillboardPawn::CameraZoom(const FInputActionValue& Value)
{
	float UpValue = Value.Get<float>();

	float NewSpringArm = CameraBoom->TargetArmLength + UpValue * ZoomSpeedScale;

	CameraBoom->TargetArmLength = FMath::Clamp(NewSpringArm, MinSpringArm, MaxSpringArm);

	UpdateBillboardScale();
}

void ANGSVMBillboardPawn::UpdateBillboardFacing()
{
	if (!BillboardMesh)
	{
		return;
	}

	APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager)
	{
		return;
	}

	const FVector CameraLocation = PC->PlayerCameraManager->GetCameraLocation();
	const FVector PlaneLocation = BillboardMesh->GetComponentLocation();

	FRotator LookAtRotation = UKismetMathLibrary::FindLookAtRotation(PlaneLocation, CameraLocation);

	// �u�O�d Yaw�A�ꦺ Pitch / Roll�A�קK�Ȥ��]�۾����C���צӫe��ɭ�
	//LookAtRotation.Pitch = 0.0f;
	LookAtRotation.Roll = 0.0f;

	const FQuat CorrectionQuat = MeshOrientationCorrection.Quaternion();
	const FQuat LookAtQuat = LookAtRotation.Quaternion();
	const FQuat FinalQuat = LookAtQuat * CorrectionQuat;

	BillboardMesh->SetWorldRotation(FinalQuat);
}

void ANGSVMBillboardPawn::UpdateBillboardScale()
{
	float CameraAspectRatio = FollowCamera->AspectRatio = 16.0f / 9.0f; // Set the camera's aspect ratio to 16:9

	float BillboardScaleFactor = (CameraBoom->TargetArmLength - 1.41) / 86.9f;

	BillboardMesh->SetWorldScale3D(FVector(BillboardScaleFactor * CameraAspectRatio, BillboardScaleFactor, 1.0f));
}

