//--------------------------------------------------------------------------------------
// File: DGSLEffectFactory.cpp
//
// THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A
// PARTICULAR PURPOSE.
//
// Copyright (c) Microsoft Corporation. All rights reserved.
//
// http://go.microsoft.com/fwlink/?LinkId=248929
//--------------------------------------------------------------------------------------

#include "pch.h"
#include "Effects.h"
#include "DemandCreate.h"
#include "SharedResourcePool.h"

#include "DDSTextureLoader.h"

#include <string.h>

#if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY != WINAPI_FAMILY_PHONE_APP)
#include "WICTextureLoader.h"
#endif

#include "BinaryReader.h"

using namespace DirectX;
using namespace Microsoft::WRL;


// Internal DGSLEffectFactory implementation class. Only one of these helpers is allocated
// per D3D device, even if there are multiple public facing DGSLEffectFactory instances.
class DGSLEffectFactory::Impl
{
public:
    Impl(_In_ ID3D11Device* device)
      : device(device), mSharing(true)
    { }

    std::shared_ptr<IEffect> CreateEffect( _In_ DGSLEffectFactory* factory, _In_ const IEffectFactory::EffectInfo& info, _In_opt_ ID3D11DeviceContext* deviceContext );
    std::shared_ptr<IEffect> CreateDGSLEffect( _In_ DGSLEffectFactory* factory, _In_ const DGSLEffectInfo& info, _In_opt_ ID3D11DeviceContext* deviceContext );
    void CreateTexture( _In_z_ const WCHAR* texture, _In_opt_ ID3D11DeviceContext* deviceContext, _Outptr_ ID3D11ShaderResourceView** textureView );
    void CreatePixelShader( _In_z_ const WCHAR* shader, _Outptr_ ID3D11PixelShader** pixelShader );

    void ReleaseCache();
    void SetSharing( bool enabled ) { mSharing = enabled; }

    ComPtr<ID3D11Device> device;

    typedef std::map< std::wstring, std::shared_ptr<IEffect> > EffectCache;
    typedef std::map< std::wstring, Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> > TextureCache;
    typedef std::map< std::wstring, Microsoft::WRL::ComPtr<ID3D11PixelShader> > ShaderCache;

    EffectCache  mEffectCache;
    TextureCache mTextureCache;
    ShaderCache  mShaderCache;

    bool mSharing;

    std::mutex mutex;

    static SharedResourcePool<ID3D11Device*, Impl> instancePool;
};


// Global instance pool.
SharedResourcePool<ID3D11Device*, DGSLEffectFactory::Impl> DGSLEffectFactory::Impl::instancePool;


_Use_decl_annotations_
std::shared_ptr<IEffect> DGSLEffectFactory::Impl::CreateEffect( DGSLEffectFactory* factory, const DGSLEffectFactory::EffectInfo& info, ID3D11DeviceContext* deviceContext )
{
    auto it = (info.name && *info.name) ? mEffectCache.find( info.name ) : mEffectCache.end();

    if ( mSharing && it != mEffectCache.end() )
    {
        return it->second;
    }
    else
    {
        std::shared_ptr<DGSLEffect> effect = std::make_shared<DGSLEffect>( device.Get() );

        effect->EnableDefaultLighting();
        effect->SetLightingEnabled(true);

        XMVECTOR color = XMLoadFloat3( &info.ambientColor );
        effect->SetAmbientColor( color );

        color = XMLoadFloat3( &info.diffuseColor );
        effect->SetDiffuseColor( color );

        effect->SetAlpha( info.alpha );

        if ( info.specularColor.x != 0 || info.specularColor.y != 0 || info.specularColor.z != 0 )
        {
            color = XMLoadFloat3( &info.specularColor );
            effect->SetSpecularColor( color );
            effect->SetSpecularPower( info.specularPower );
        }

        if ( info.emissiveColor.x != 0 || info.emissiveColor.y != 0 || info.emissiveColor.z != 0 )
        {
            color = XMLoadFloat3( &info.emissiveColor );
            effect->SetEmissiveColor( color );
        }

        if ( info.texture && *info.texture )
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

            factory->CreateTexture( info.texture, deviceContext, srv.GetAddressOf() );

            effect->SetTexture( srv.Get() );
            effect->SetTextureEnabled(true);
        }

        if ( mSharing && info.name && *info.name && it == mEffectCache.end() )
        {
            std::lock_guard<std::mutex> lock(mutex);
            mEffectCache.insert( EffectCache::value_type( info.name, effect) );
        }

        return effect;
    }
}


_Use_decl_annotations_
std::shared_ptr<IEffect> DGSLEffectFactory::Impl::CreateDGSLEffect( DGSLEffectFactory* factory, const DGSLEffectFactory::DGSLEffectInfo& info, ID3D11DeviceContext* deviceContext )
{
    auto it = (info.name && *info.name) ? mEffectCache.find( info.name ) : mEffectCache.end();

    if ( mSharing && it != mEffectCache.end() )
    {
        return it->second;
    }
    else
    {
        std::shared_ptr<DGSLEffect> effect;

        bool lighting = true;
        bool allowSpecular = true;

        if ( !info.pixelShader || !*info.pixelShader )
        {
            effect = std::make_shared<DGSLEffect>( device.Get() );
        }
        else
        {
            wchar_t root[ MAX_PATH ] = {0};
            auto last = wcsrchr( info.pixelShader, '_' );
            if ( last )
            {
                wcscpy_s( root, last+1 );
            }
            else
            {
                wcscpy_s( root, info.pixelShader );
            }

            auto first = wcschr( root, '.' );
            if ( first )
                *first = 0;

            if ( !_wcsicmp( root, L"lambert" ) )
            {
                allowSpecular = false;
                effect= std::make_shared<DGSLEffect>( device.Get() );
            }
            else if ( !_wcsicmp( root, L"phong" ) )
            {
                effect= std::make_shared<DGSLEffect>( device.Get() );
            }
            else if ( !_wcsicmp( root, L"unlit" ) )
            {
                lighting = false;
                effect= std::make_shared<DGSLEffect>( device.Get() );
            }
            else if ( device->GetFeatureLevel() < D3D_FEATURE_LEVEL_10_0 )
            {
                // DGSL shaders are not compatible with Feature Level 9.x, use fallback shader
                wcscat_s( root, L".cso" );

                Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
                factory->CreatePixelShader( root, ps.GetAddressOf() );

                effect= std::make_shared<DGSLEffect>( device.Get(), ps.Get() );
            }
            else
            {
                // Create DGSL shader and use it for the effect
                Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
                factory->CreatePixelShader( info.pixelShader, ps.GetAddressOf() );

                effect= std::make_shared<DGSLEffect>( device.Get(), ps.Get() );
            }
        }

        if ( lighting )
        {
            effect->EnableDefaultLighting();
            effect->SetLightingEnabled(true);
        }

        XMVECTOR color = XMLoadFloat3( &info.ambientColor );
        effect->SetAmbientColor( color );

        color = XMLoadFloat3( &info.diffuseColor );
        effect->SetDiffuseColor( color );
        effect->SetAlpha( info.alpha );

        if ( allowSpecular
             && ( info.specularColor.x != 0 || info.specularColor.y != 0 || info.specularColor.z != 0 ) )
        {
            color = XMLoadFloat3( &info.specularColor );
            effect->SetSpecularColor( color );
            effect->SetSpecularPower( info.specularPower );
        }
        else
        {
            effect->DisableSpecular();
        }

        if ( info.emissiveColor.x != 0 || info.emissiveColor.y != 0 || info.emissiveColor.z != 0 )
        {
            color = XMLoadFloat3( &info.emissiveColor );
            effect->SetEmissiveColor( color );
        }

        if ( info.texture && *info.texture )
        {
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

            factory->CreateTexture( info.texture, deviceContext, srv.GetAddressOf() );

            effect->SetTexture( srv.Get() );
            effect->SetTextureEnabled(true);
        }

        for( int j = 0; j < 7; ++j )
        {
            if ( info.textures[j] && *info.textures[j] )
            {
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;

                factory->CreateTexture( info.textures[j], deviceContext, srv.GetAddressOf() );

                effect->SetTexture( j+1, srv.Get() );
                effect->SetTextureEnabled(true);
            }
        }

        if ( mSharing && info.name && *info.name && it == mEffectCache.end() )
        {
            std::lock_guard<std::mutex> lock(mutex);
            mEffectCache.insert( EffectCache::value_type( info.name, effect) );
        }

        return effect;
    }
}


_Use_decl_annotations_
void DGSLEffectFactory::Impl::CreateTexture( const WCHAR* name, ID3D11DeviceContext* deviceContext, ID3D11ShaderResourceView** textureView )
{
    if ( !name || !textureView )
        throw std::exception("invalid arguments");

    auto it = mTextureCache.find( name );

    if ( mSharing && it != mTextureCache.end() )
    {
        ID3D11ShaderResourceView* srv = it->second.Get();
        srv->AddRef();
        *textureView = srv;
    }
    else
    {
#if !defined(WINAPI_FAMILY) || (WINAPI_FAMILY != WINAPI_FAMILY_PHONE_APP)
        WCHAR ext[_MAX_EXT];
        _wsplitpath_s( name, nullptr, 0, nullptr, 0, nullptr, 0, ext, _MAX_EXT );

        if ( _wcsicmp( ext, L".dds" ) == 0 )
        {
            ThrowIfFailed(
                CreateDDSTextureFromFile( device.Get(), name, nullptr, textureView )
                );
        }
        else if ( deviceContext )
        {
            std::lock_guard<std::mutex> lock(mutex);
            DirectX::ThrowIfFailed(
                CreateWICTextureFromFile( device.Get(), deviceContext, name, nullptr, textureView )
                );
        }
        else
        {
            DirectX::ThrowIfFailed(
                CreateWICTextureFromFile( device.Get(), nullptr, name, nullptr, textureView )
                );
        }
#else
        UNREFERENCED_PARAMETER( deviceContext );
        ThrowIfFailed(
            CreateDDSTextureFromFile( device.Get(), name, nullptr, textureView ) );
#endif

        if ( mSharing && *name && it == mTextureCache.end() )
        {   
            std::lock_guard<std::mutex> lock(mutex);
            mTextureCache.insert( TextureCache::value_type( name, *textureView ) );
        }
    }
}


_Use_decl_annotations_
void DGSLEffectFactory::Impl::CreatePixelShader( const WCHAR* name, ID3D11PixelShader** pixelShader )
{
    if ( !name || !pixelShader )
        throw std::exception("invalid arguments");

    auto it = mShaderCache.find( name );

    if ( mSharing && it != mShaderCache.end() )
    {
        ID3D11PixelShader* ps = it->second.Get();
        ps->AddRef();
        *pixelShader = ps;
    }
    else
    {
        size_t dataSize = 0;
        std::unique_ptr<uint8_t[]> data;
        ThrowIfFailed(
            BinaryReader::ReadEntireFile( name, data, &dataSize )
        );
       
        ThrowIfFailed(
            device->CreatePixelShader( data.get(), dataSize, nullptr, pixelShader ) );

        if ( mSharing && *name && it == mShaderCache.end() )
        {   
            std::lock_guard<std::mutex> lock(mutex);
            mShaderCache.insert( ShaderCache::value_type( name, *pixelShader ) );
        }
    }
}


void DGSLEffectFactory::Impl::ReleaseCache()
{
    std::lock_guard<std::mutex> lock(mutex);
    mEffectCache.clear();
    mTextureCache.clear();
    mShaderCache.clear();
}



//--------------------------------------------------------------------------------------
// DGSLEffectFactory
//--------------------------------------------------------------------------------------

DGSLEffectFactory::DGSLEffectFactory(_In_ ID3D11Device* device)
    : pImpl(Impl::instancePool.DemandCreate(device))
{
}

DGSLEffectFactory::~DGSLEffectFactory()
{
}


DGSLEffectFactory::DGSLEffectFactory(DGSLEffectFactory&& moveFrom)
    : pImpl(std::move(moveFrom.pImpl))
{
}

DGSLEffectFactory& DGSLEffectFactory::operator= (DGSLEffectFactory&& moveFrom)
{
    pImpl = std::move(moveFrom.pImpl);
    return *this;
}


// IEffectFactory methods
_Use_decl_annotations_
std::shared_ptr<IEffect> DGSLEffectFactory::CreateEffect( const EffectInfo& info, ID3D11DeviceContext* deviceContext )
{
    return pImpl->CreateEffect( this, info, deviceContext );
}

_Use_decl_annotations_
void DGSLEffectFactory::CreateTexture( const WCHAR* name, ID3D11DeviceContext* deviceContext, ID3D11ShaderResourceView** textureView )
{
    return pImpl->CreateTexture( name, deviceContext, textureView );
}


// DGSL methods.
_Use_decl_annotations_
std::shared_ptr<IEffect> DGSLEffectFactory::CreateDGSLEffect( const DGSLEffectInfo& info, ID3D11DeviceContext* deviceContext )
{
    return pImpl->CreateDGSLEffect( this, info, deviceContext );
}


_Use_decl_annotations_
void DGSLEffectFactory::CreatePixelShader( const WCHAR* shader, ID3D11PixelShader** pixelShader )
{
    pImpl->CreatePixelShader( shader, pixelShader );
}


// Settings
void DGSLEffectFactory::ReleaseCache()
{
    pImpl->ReleaseCache();
}

void DGSLEffectFactory::SetSharing( bool enabled )
{
    pImpl->SetSharing( enabled );
}
