#!/bin/bash

# CI/CD APK signing script for Rockbox
# This script signs the APK non-interactively for use in pipelines
# Usage: ./sign_apk_ci.sh [BUILDDIR] [KEYSTORE_PATH] [KEY_ALIAS] [KEYSTORE_PASSWORD] [KEY_PASSWORD]
# Can be run from build directory or rockbox root directory

set -e

# Detect if we're running from build directory or root directory
if [ -f "android/android.make" ]; then
    # Running from rockbox root directory
    ROCKBOX_ROOT="."
    BUILDDIR=${1:-"build"}
elif [ -f "../android.make" ]; then
    # Running from build directory
    ROCKBOX_ROOT="../../"
    BUILDDIR="."
else
    echo "Error: This script must be run from the Rockbox root directory or a build directory"
    echo "Current directory: $(pwd)"
    echo "Expected to find android/android.make or ../android/android.make"
    exit 1
fi

# Default values
KEYSTORE_PATH=${2:-"$HOME/.android/release.keystore"}
KEY_ALIAS=${3:-"rockbox_release"}
KEYSTORE_PASSWORD=${4:-"rockbox123"}
KEY_PASSWORD=${5:-"rockbox123"}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_status() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if we're in the right directory structure
if [ ! -f "$ROCKBOX_ROOT/android/android.make" ]; then
    print_error "android.make not found at $ROCKBOX_ROOT/android/android.make"
    print_error "This script must be run from the Rockbox root directory or a build directory"
    exit 1
fi

# Check if BUILDDIR exists
if [ ! -d "$BUILDDIR" ]; then
    print_error "Build directory '$BUILDDIR' does not exist"
    print_error "Please run 'make unsigned-apk' first"
    exit 1
fi

# Check if the unsigned APK exists
UNSIGNED_APK="$BUILDDIR/bin/_rockbox.apk"
if [ ! -f "$UNSIGNED_APK" ]; then
    print_error "Unsigned APK not found at '$UNSIGNED_APK'"
    print_error "Please run 'make unsigned-apk' first"
    exit 1
fi

# Check Java version
JAVA_VERSION=$(java -version 2>&1 | head -n 1 | cut -d'"' -f2 | cut -d'.' -f1)
if [ "$JAVA_VERSION" -lt "8" ]; then
    print_error "Java 8 or higher is required. Found Java $JAVA_VERSION"
    exit 1
fi

print_status "Using Java version: $(java -version 2>&1 | head -n 1)"
print_status "Running from: $(pwd)"
print_status "Build directory: $BUILDDIR"

# Create keystore directory if it doesn't exist
KEYSTORE_DIR=$(dirname "$KEYSTORE_PATH")
if [ ! -d "$KEYSTORE_DIR" ]; then
    print_status "Creating keystore directory: $KEYSTORE_DIR"
    mkdir -p "$KEYSTORE_DIR"
fi

# Create keystore if it doesn't exist (non-interactive)
if [ ! -f "$KEYSTORE_PATH" ]; then
    print_status "Creating new keystore at: $KEYSTORE_PATH"
    
    # Create keystore with provided passwords
    keytool -genkey -v \
        -keystore "$KEYSTORE_PATH" \
        -alias "$KEY_ALIAS" \
        -keyalg RSA \
        -keysize 4096 \
        -validity 10000 \
        -sigalg SHA256withRSA \
        -storetype PKCS12 \
        -storepass "$KEYSTORE_PASSWORD" \
        -keypass "$KEY_PASSWORD" \
        -dname "CN=Rockbox Release, OU=Rockbox, O=Rockbox Project, L=Unknown, ST=Unknown, C=US" \
        -noprompt
    
    if [ $? -ne 0 ]; then
        print_error "Failed to create keystore"
        exit 1
    fi
else
    print_status "Using existing keystore: $KEYSTORE_PATH"
fi

# Check if apksigner is available (Android SDK Build Tools 24.0.3+)
APKSIGNER=""
if command -v apksigner >/dev/null 2>&1; then
    APKSIGNER="apksigner"
elif [ -n "$ANDROID_SDK_PATH" ] && [ -f "$ANDROID_SDK_PATH/build-tools/$(ls -1 "$ANDROID_SDK_PATH/build-tools" | tail -1)/apksigner" ]; then
    APKSIGNER="$ANDROID_SDK_PATH/build-tools/$(ls -1 "$ANDROID_SDK_PATH/build-tools" | tail -1)/apksigner"
fi

# Sign the APK
SIGNED_APK="$BUILDDIR/rockbox_signed.apk"
TEMP_APK="$BUILDDIR/temp_signed.apk"

print_status "Signing APK..."

if [ -n "$APKSIGNER" ]; then
    print_status "Using apksigner (modern Android signing tool)"
    
    # Use apksigner for modern signing
    "$APKSIGNER" sign \
        --ks "$KEYSTORE_PATH" \
        --ks-pass "pass:$KEYSTORE_PASSWORD" \
        --ks-key-alias "$KEY_ALIAS" \
        --key-pass "pass:$KEY_PASSWORD" \
        --v1-signing-enabled true \
        --v2-signing-enabled true \
        --v3-signing-enabled true \
        --v4-signing-enabled false \
        --out "$SIGNED_APK" \
        "$UNSIGNED_APK"
    
    if [ $? -ne 0 ]; then
        print_error "apksigner failed"
        exit 1
    fi
else
    print_status "Using jarsigner (fallback method)"
    
    # Fallback to jarsigner with modern algorithms
    jarsigner -verbose \
        -keystore "$KEYSTORE_PATH" \
        -storepass "$KEYSTORE_PASSWORD" \
        -keypass "$KEY_PASSWORD" \
        -sigalg SHA256withRSA \
        -digestalg SHA-256 \
        -signedjar "$TEMP_APK" \
        "$UNSIGNED_APK" \
        "$KEY_ALIAS"
    
    if [ $? -ne 0 ]; then
        print_error "jarsigner failed"
        exit 1
    fi
    
    # Optimize the APK
    if command -v zipalign >/dev/null 2>&1; then
        print_status "Optimizing APK with zipalign"
        zipalign -v 4 "$TEMP_APK" "$SIGNED_APK"
        rm -f "$TEMP_APK"
    else
        mv "$TEMP_APK" "$SIGNED_APK"
    fi
fi

# Verify the signature
print_status "Verifying signature..."
if [ -n "$APKSIGNER" ]; then
    "$APKSIGNER" verify --verbose "$SIGNED_APK"
else
    jarsigner -verify -verbose -certs "$SIGNED_APK"
fi

if [ $? -eq 0 ]; then
    print_status "APK successfully signed and verified!"
    print_status "Signed APK location: $SIGNED_APK"
    
    # Show APK info
    print_status "APK file size: $(du -h "$SIGNED_APK" | cut -f1)"
    
    # Replace the original APK
    cp "$SIGNED_APK" "$BUILDDIR/rockbox.apk"
    print_status "Original APK replaced with signed version"
else
    print_error "Signature verification failed"
    exit 1
fi

print_status "Signing process completed successfully!"
