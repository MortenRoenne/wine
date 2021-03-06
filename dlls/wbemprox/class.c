/*
 * Copyright 2012 Hans Leidekker for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define COBJMACROS

#include "config.h"
#include <stdarg.h>

#include "windef.h"
#include "winbase.h"
#include "objbase.h"
#include "wbemcli.h"

#include "wine/debug.h"
#include "wbemprox_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(wbemprox);

struct enum_class_object
{
    IEnumWbemClassObject IEnumWbemClassObject_iface;
    LONG refs;
    struct query *query;
    UINT index;
};

static inline struct enum_class_object *impl_from_IEnumWbemClassObject(
    IEnumWbemClassObject *iface )
{
    return CONTAINING_RECORD(iface, struct enum_class_object, IEnumWbemClassObject_iface);
}

static ULONG WINAPI enum_class_object_AddRef(
    IEnumWbemClassObject *iface )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );
    return InterlockedIncrement( &ec->refs );
}

static ULONG WINAPI enum_class_object_Release(
    IEnumWbemClassObject *iface )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );
    LONG refs = InterlockedDecrement( &ec->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", ec);
        release_query( ec->query );
        heap_free( ec );
    }
    return refs;
}

static HRESULT WINAPI enum_class_object_QueryInterface(
    IEnumWbemClassObject *iface,
    REFIID riid,
    void **ppvObject )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );

    TRACE("%p, %s, %p\n", ec, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IEnumWbemClassObject ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = ec;
    }
    else if ( IsEqualGUID( riid, &IID_IClientSecurity ) )
    {
        *ppvObject = &client_security;
        return S_OK;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IEnumWbemClassObject_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI enum_class_object_Reset(
    IEnumWbemClassObject *iface )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );

    TRACE("%p\n", iface);

    ec->index = 0;
    return WBEM_S_NO_ERROR;
}

static HRESULT WINAPI enum_class_object_Next(
    IEnumWbemClassObject *iface,
    LONG lTimeout,
    ULONG uCount,
    IWbemClassObject **apObjects,
    ULONG *puReturned )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );
    struct view *view = ec->query->view;
    HRESULT hr;

    TRACE("%p, %d, %u, %p, %p\n", iface, lTimeout, uCount, apObjects, puReturned);

    if (!uCount) return WBEM_S_FALSE;
    if (!apObjects || !puReturned) return WBEM_E_INVALID_PARAMETER;
    if (lTimeout != WBEM_INFINITE) FIXME("timeout not supported\n");

    *puReturned = 0;
    if (ec->index + uCount > view->count) return WBEM_S_FALSE;

    hr = WbemClassObject_create( NULL, iface, ec->index, (void **)apObjects );
    if (hr != S_OK) return hr;

    ec->index++;
    *puReturned = 1;
    if (ec->index == view->count) return WBEM_S_FALSE;
    if (uCount > 1) return WBEM_S_TIMEDOUT;
    return WBEM_S_NO_ERROR;
}

static HRESULT WINAPI enum_class_object_NextAsync(
    IEnumWbemClassObject *iface,
    ULONG uCount,
    IWbemObjectSink *pSink )
{
    FIXME("%p, %u, %p\n", iface, uCount, pSink);
    return E_NOTIMPL;
}

static HRESULT WINAPI enum_class_object_Clone(
    IEnumWbemClassObject *iface,
    IEnumWbemClassObject **ppEnum )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );

    TRACE("%p, %p\n", iface, ppEnum);

    return EnumWbemClassObject_create( NULL, ec->query, (void **)ppEnum );
}

static HRESULT WINAPI enum_class_object_Skip(
    IEnumWbemClassObject *iface,
    LONG lTimeout,
    ULONG nCount )
{
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( iface );
    struct view *view = ec->query->view;

    TRACE("%p, %d, %u\n", iface, lTimeout, nCount);

    if (lTimeout != WBEM_INFINITE) FIXME("timeout not supported\n");

    if (ec->index + nCount >= view->count)
    {
        ec->index = view->count - 1;
        return WBEM_S_FALSE;
    }
    ec->index += nCount;
    return WBEM_S_NO_ERROR;
}

static const IEnumWbemClassObjectVtbl enum_class_object_vtbl =
{
    enum_class_object_QueryInterface,
    enum_class_object_AddRef,
    enum_class_object_Release,
    enum_class_object_Reset,
    enum_class_object_Next,
    enum_class_object_NextAsync,
    enum_class_object_Clone,
    enum_class_object_Skip
};

HRESULT EnumWbemClassObject_create(
    IUnknown *pUnkOuter, struct query *query, LPVOID *ppObj )
{
    struct enum_class_object *ec;

    TRACE("%p, %p\n", pUnkOuter, ppObj);

    ec = heap_alloc( sizeof(*ec) );
    if (!ec) return E_OUTOFMEMORY;

    ec->IEnumWbemClassObject_iface.lpVtbl = &enum_class_object_vtbl;
    ec->refs  = 1;
    ec->query = query;
    addref_query( query );
    ec->index = 0;

    *ppObj = &ec->IEnumWbemClassObject_iface;

    TRACE("returning iface %p\n", *ppObj);
    return S_OK;
}

struct class_object
{
    IWbemClassObject IWbemClassObject_iface;
    LONG refs;
    IEnumWbemClassObject *iter;
    UINT index;
};

static inline struct class_object *impl_from_IWbemClassObject(
    IWbemClassObject *iface )
{
    return CONTAINING_RECORD(iface, struct class_object, IWbemClassObject_iface);
}

static ULONG WINAPI class_object_AddRef(
    IWbemClassObject *iface )
{
    struct class_object *co = impl_from_IWbemClassObject( iface );
    return InterlockedIncrement( &co->refs );
}

static ULONG WINAPI class_object_Release(
    IWbemClassObject *iface )
{
    struct class_object *co = impl_from_IWbemClassObject( iface );
    LONG refs = InterlockedDecrement( &co->refs );
    if (!refs)
    {
        TRACE("destroying %p\n", co);
        if (co->iter) IEnumWbemClassObject_Release( co->iter );
        heap_free( co );
    }
    return refs;
}

static HRESULT WINAPI class_object_QueryInterface(
    IWbemClassObject *iface,
    REFIID riid,
    void **ppvObject )
{
    struct class_object *co = impl_from_IWbemClassObject( iface );

    TRACE("%p, %s, %p\n", co, debugstr_guid( riid ), ppvObject );

    if ( IsEqualGUID( riid, &IID_IWbemClassObject ) ||
         IsEqualGUID( riid, &IID_IUnknown ) )
    {
        *ppvObject = co;
    }
    else
    {
        FIXME("interface %s not implemented\n", debugstr_guid(riid));
        return E_NOINTERFACE;
    }
    IWbemClassObject_AddRef( iface );
    return S_OK;
}

static HRESULT WINAPI class_object_GetQualifierSet(
    IWbemClassObject *iface,
    IWbemQualifierSet **ppQualSet )
{
    FIXME("%p, %p\n", iface, ppQualSet);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_Get(
    IWbemClassObject *iface,
    LPCWSTR wszName,
    LONG lFlags,
    VARIANT *pVal,
    CIMTYPE *pType,
    LONG *plFlavor )
{
    struct class_object *co = impl_from_IWbemClassObject( iface );
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( co->iter );
    struct view *view = ec->query->view;

    TRACE("%p, %s, %08x, %p, %p, %p\n", iface, debugstr_w(wszName), lFlags, pVal, pType, plFlavor);

    return get_propval( view, co->index, wszName, pVal, pType, plFlavor );
}

static HRESULT WINAPI class_object_Put(
    IWbemClassObject *iface,
    LPCWSTR wszName,
    LONG lFlags,
    VARIANT *pVal,
    CIMTYPE Type )
{
    FIXME("%p, %s, %08x, %p, %u\n", iface, debugstr_w(wszName), lFlags, pVal, Type);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_Delete(
    IWbemClassObject *iface,
    LPCWSTR wszName )
{
    FIXME("%p, %s\n", iface, debugstr_w(wszName));
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetNames(
    IWbemClassObject *iface,
    LPCWSTR wszQualifierName,
    LONG lFlags,
    VARIANT *pQualifierVal,
    SAFEARRAY **pNames )
{
    struct class_object *co = impl_from_IWbemClassObject( iface );
    struct enum_class_object *ec = impl_from_IEnumWbemClassObject( co->iter );

    TRACE("%p, %s, %08x, %p, %p\n", iface, debugstr_w(wszQualifierName), lFlags, pQualifierVal, pNames);

    if (wszQualifierName || pQualifierVal)
    {
        FIXME("qualifier not supported\n");
        return E_NOTIMPL;
    }
    if (lFlags != WBEM_FLAG_ALWAYS)
    {
        FIXME("flags %08x not supported\n", lFlags);
        return E_NOTIMPL;
    }
    return get_properties( ec->query->view, pNames );
}

static HRESULT WINAPI class_object_BeginEnumeration(
    IWbemClassObject *iface,
    LONG lEnumFlags )
{
    FIXME("%p, %08x\n", iface, lEnumFlags);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_Next(
    IWbemClassObject *iface,
    LONG lFlags,
    BSTR *strName,
    VARIANT *pVal,
    CIMTYPE *pType,
    LONG *plFlavor )
{
    FIXME("%p, %08x, %p, %p, %p, %p\n", iface, lFlags, strName, pVal, pType, plFlavor);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_EndEnumeration(
    IWbemClassObject *iface )
{
    FIXME("%p\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetPropertyQualifierSet(
    IWbemClassObject *iface,
    LPCWSTR wszProperty,
    IWbemQualifierSet **ppQualSet )
{
    FIXME("%p, %s, %p\n", iface, debugstr_w(wszProperty), ppQualSet);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_Clone(
    IWbemClassObject *iface,
    IWbemClassObject **ppCopy )
{
    FIXME("%p, %p\n", iface, ppCopy);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetObjectText(
    IWbemClassObject *iface,
    LONG lFlags,
    BSTR *pstrObjectText )
{
    FIXME("%p, %08x, %p\n", iface, lFlags, pstrObjectText);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_SpawnDerivedClass(
    IWbemClassObject *iface,
    LONG lFlags,
    IWbemClassObject **ppNewClass )
{
    FIXME("%p, %08x, %p\n", iface, lFlags, ppNewClass);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_SpawnInstance(
    IWbemClassObject *iface,
    LONG lFlags,
    IWbemClassObject **ppNewInstance )
{
    FIXME("%p, %08x, %p\n", iface, lFlags, ppNewInstance);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_CompareTo(
    IWbemClassObject *iface,
    LONG lFlags,
    IWbemClassObject *pCompareTo )
{
    FIXME("%p, %08x, %p\n", iface, lFlags, pCompareTo);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetPropertyOrigin(
    IWbemClassObject *iface,
    LPCWSTR wszName,
    BSTR *pstrClassName )
{
    FIXME("%p, %s, %p\n", iface, debugstr_w(wszName), pstrClassName);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_InheritsFrom(
    IWbemClassObject *iface,
    LPCWSTR strAncestor )
{
    FIXME("%p, %s\n", iface, debugstr_w(strAncestor));
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetMethod(
    IWbemClassObject *iface,
    LPCWSTR wszName,
    LONG lFlags,
    IWbemClassObject **ppInSignature,
    IWbemClassObject **ppOutSignature )
{
    FIXME("%p, %s, %08x, %p, %p\n", iface, debugstr_w(wszName), lFlags, ppInSignature, ppOutSignature);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_PutMethod(
    IWbemClassObject *iface,
    LPCWSTR wszName,
    LONG lFlags,
    IWbemClassObject *pInSignature,
    IWbemClassObject *pOutSignature )
{
    FIXME("%p, %s, %08x, %p, %p\n", iface, debugstr_w(wszName), lFlags, pInSignature, pOutSignature);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_DeleteMethod(
    IWbemClassObject *iface,
    LPCWSTR wszName )
{
    FIXME("%p, %s\n", iface, debugstr_w(wszName));
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_BeginMethodEnumeration(
    IWbemClassObject *iface,
    LONG lEnumFlags)
{
    FIXME("%p, %08x\n", iface, lEnumFlags);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_NextMethod(
    IWbemClassObject *iface,
    LONG lFlags,
    BSTR *pstrName,
    IWbemClassObject **ppInSignature,
    IWbemClassObject **ppOutSignature)
{
    FIXME("%p, %08x, %p, %p, %p\n", iface, lFlags, pstrName, ppInSignature, ppOutSignature);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_EndMethodEnumeration(
    IWbemClassObject *iface )
{
    FIXME("%p\n", iface);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetMethodQualifierSet(
    IWbemClassObject *iface,
    LPCWSTR wszMethod,
    IWbemQualifierSet **ppQualSet)
{
    FIXME("%p, %s, %p\n", iface, debugstr_w(wszMethod), ppQualSet);
    return E_NOTIMPL;
}

static HRESULT WINAPI class_object_GetMethodOrigin(
    IWbemClassObject *iface,
    LPCWSTR wszMethodName,
    BSTR *pstrClassName)
{
    FIXME("%p, %s, %p\n", iface, debugstr_w(wszMethodName), pstrClassName);
    return E_NOTIMPL;
}

static const IWbemClassObjectVtbl class_object_vtbl =
{
    class_object_QueryInterface,
    class_object_AddRef,
    class_object_Release,
    class_object_GetQualifierSet,
    class_object_Get,
    class_object_Put,
    class_object_Delete,
    class_object_GetNames,
    class_object_BeginEnumeration,
    class_object_Next,
    class_object_EndEnumeration,
    class_object_GetPropertyQualifierSet,
    class_object_Clone,
    class_object_GetObjectText,
    class_object_SpawnDerivedClass,
    class_object_SpawnInstance,
    class_object_CompareTo,
    class_object_GetPropertyOrigin,
    class_object_InheritsFrom,
    class_object_GetMethod,
    class_object_PutMethod,
    class_object_DeleteMethod,
    class_object_BeginMethodEnumeration,
    class_object_NextMethod,
    class_object_EndMethodEnumeration,
    class_object_GetMethodQualifierSet,
    class_object_GetMethodOrigin
};

HRESULT WbemClassObject_create(
    IUnknown *pUnkOuter, IEnumWbemClassObject *iter, UINT index, LPVOID *ppObj )
{
    struct class_object *co;

    TRACE("%p, %p\n", pUnkOuter, ppObj);

    co = heap_alloc( sizeof(*co) );
    if (!co) return E_OUTOFMEMORY;

    co->IWbemClassObject_iface.lpVtbl = &class_object_vtbl;
    co->refs  = 1;
    co->iter  = iter;
    co->index = index;
    if (iter) IEnumWbemClassObject_AddRef( iter );

    *ppObj = &co->IWbemClassObject_iface;

    TRACE("returning iface %p\n", *ppObj);
    return S_OK;
}
