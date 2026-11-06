/*
* PROJECT:     ReactOS Win32k subsystem
* LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
* FILE:        win32ss/gdi/eng/multidisp.c
* PURPOSE:     Multi-Display driver
* PROGRAMMERS:
*/

/* INCLUDES *******************************************************************/

#include <win32k.h>
#define NDEBUG
#include <debug.h>

#define MULTI_MAX_CLONE_TARGETS 4
#define MULTI_MAX_DAMAGE_RECTS  32

typedef struct _MULTI_CLONE
{
    HSEMAPHORE hsem;
    PPDEVOBJ ppdevSource;
    DRIVER_FUNCTIONS pfnSource;
    ULONG cTargets;
    PPDEVOBJ appdevTarget[MULTI_MAX_CLONE_TARGETS];
    BOOL abFill[MULTI_MAX_CLONE_TARGETS];
    ULONG cDamage;
    BOOL bFullDamage;
    RECTL arclDamage[MULTI_MAX_DAMAGE_RECTS];
} MULTI_CLONE;

static MULTI_CLONE gMultiClone;

BOOL
APIENTRY
MultiEnableDriver(
    _In_ ULONG iEngineVersion,
    _In_ ULONG cj,
    _Inout_bytecount_(cj) PDRVENABLEDATA pded)
{
    UNIMPLEMENTED;
    return FALSE;
}

NTSTATUS
MultiInitialize(VOID)
{
    gMultiClone.hsem = EngCreateSemaphore();
    return gMultiClone.hsem ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static
BOOL
MultiIsSourcePrimary(
    _In_opt_ SURFOBJ *pso)
{
    PPDEVOBJ ppdev = gMultiClone.ppdevSource;

    return pso != NULL && ppdev != NULL && ppdev->pSurface != NULL && pso == &ppdev->pSurface->SurfObj;
}

static
VOID
MultiAddDamage(
    _In_ SURFOBJ *pso,
    _In_opt_ CLIPOBJ *pco,
    _In_opt_ const RECTL *prcl)
{
    RECTL rcl, rclSurface;
    ULONG i;

    if (!MultiIsSourcePrimary(pso))
        return;

    rclSurface.left = 0;
    rclSurface.top = 0;
    rclSurface.right = pso->sizlBitmap.cx;
    rclSurface.bottom = pso->sizlBitmap.cy;
    rcl = prcl ? *prcl : rclSurface;
    RECTL_vMakeWellOrdered(&rcl);
    if (pco && pco->iDComplexity != DC_TRIVIAL && !RECTL_bIntersectRect(&rcl, &rcl, &pco->rclBounds))
        return;
    if (!RECTL_bIntersectRect(&rcl, &rcl, &rclSurface))
        return;

    EngAcquireSemaphore(gMultiClone.hsem);
    if (!gMultiClone.bFullDamage)
    {
        for (i = 0; i < gMultiClone.cDamage; i++)
        {
            RECTL rclUnion;

            RECTL_bUnionRect(&rclUnion, &gMultiClone.arclDamage[i], &rcl);
            if ((rclUnion.right - rclUnion.left) * (rclUnion.bottom - rclUnion.top) <=
                (gMultiClone.arclDamage[i].right - gMultiClone.arclDamage[i].left) *
                (gMultiClone.arclDamage[i].bottom - gMultiClone.arclDamage[i].top) +
                (rcl.right - rcl.left) * (rcl.bottom - rcl.top))
            {
                gMultiClone.arclDamage[i] = rclUnion;
                break;
            }
        }
        if (i == gMultiClone.cDamage)
        {
            if (gMultiClone.cDamage < MULTI_MAX_DAMAGE_RECTS)
            {
                gMultiClone.arclDamage[gMultiClone.cDamage++] = rcl;
            }
            else
            {
                RECTL_bUnionRect(&gMultiClone.arclDamage[0], &gMultiClone.arclDamage[0], &rcl);
            }
        }
    }
    EngReleaseSemaphore(gMultiClone.hsem);
}

static
VOID
MultiGetTargetRect(
    _In_ const SIZEL *psizlSource,
    _In_ const SIZEL *psizlTarget,
    _Out_ PRECTL prclTarget)
{
    LONG cx = psizlTarget->cx, cy = psizlTarget->cy;

    if ((LONGLONG)psizlTarget->cx * psizlSource->cy > (LONGLONG)psizlTarget->cy * psizlSource->cx)
        cx = (LONG)((LONGLONG)psizlSource->cx * psizlTarget->cy / psizlSource->cy);
    else
        cy = (LONG)((LONGLONG)psizlSource->cy * psizlTarget->cx / psizlSource->cx);

    prclTarget->left = (psizlTarget->cx - cx) / 2;
    prclTarget->top = (psizlTarget->cy - cy) / 2;
    prclTarget->right = prclTarget->left + cx;
    prclTarget->bottom = prclTarget->top + cy;
}

static
VOID
MultiCopyToTarget(
    _In_ PSURFACE psurfSource,
    _In_ PPDEVOBJ ppdevTarget,
    _In_ BOOL bFill,
    _In_reads_(cRects) const RECTL *prclSource,
    _In_ ULONG cRects)
{
    PSURFACE psurfTarget;
    SURFOBJ *psoSource = &psurfSource->SurfObj, *psoTarget;
    PPDEVOBJ ppdev = ppdevTarget;
    EXLATEOBJ exlo;
    RECTL rclView, rclDirty, rclDest, rclSrc;
    SIZEL sizlTarget;
    POINTL ptlSrc;
    BOOL bIdentity, bDirty = FALSE;
    ULONG i;

    EngAcquireSemaphore(ppdev->hsemDevLock);
    psurfTarget = ppdev->pSurface;
    if (psurfTarget == NULL || (ppdev->flFlags & PDEV_DISABLED) ||
        psoSource->sizlBitmap.cx <= 0 || psoSource->sizlBitmap.cy <= 0)
    {
        EngReleaseSemaphore(ppdev->hsemDevLock);
        return;
    }
    psoTarget = &psurfTarget->SurfObj;
    sizlTarget = psoTarget->sizlBitmap;
    MultiGetTargetRect(&psoSource->sizlBitmap, &sizlTarget, &rclView);
    bIdentity = rclView.right - rclView.left == psoSource->sizlBitmap.cx &&
                rclView.bottom - rclView.top == psoSource->sizlBitmap.cy;

    if (bFill)
    {
        rclDest.left = rclDest.top = 0;
        rclDest.right = sizlTarget.cx;
        rclDest.bottom = sizlTarget.cy;
        if ((psurfTarget->flags & HOOK_BITBLT) && ppdev->pfn.BitBlt)
        {
            ppdev->pfn.BitBlt(psoTarget, NULL, NULL, (CLIPOBJ *)&gxcoTrivial, NULL, &rclDest,
                              NULL, NULL, NULL, NULL, ROP4_FROM_INDEX(R3_OPINDEX_BLACKNESS));
        }
        else
        {
            EngBitBlt(psoTarget, NULL, NULL, (CLIPOBJ *)&gxcoTrivial, NULL, &rclDest,
                      NULL, NULL, NULL, NULL, ROP4_FROM_INDEX(R3_OPINDEX_BLACKNESS));
        }
        rclDirty = rclDest;
        bDirty = TRUE;
    }

    EXLATEOBJ_vInitialize(&exlo, psurfSource->ppal, psurfTarget->ppal, 0, 0, 0);
    for (i = 0; i < cRects; i++)
    {
        rclSrc = prclSource[i];
        if (bIdentity)
        {
            rclDest.left = rclView.left + rclSrc.left;
            rclDest.top = rclView.top + rclSrc.top;
            rclDest.right = rclView.left + rclSrc.right;
            rclDest.bottom = rclView.top + rclSrc.bottom;
            ptlSrc.x = rclSrc.left;
            ptlSrc.y = rclSrc.top;
            if ((psurfTarget->flags & HOOK_COPYBITS) && ppdev->pfn.CopyBits)
                ppdev->pfn.CopyBits(psoTarget, psoSource, (CLIPOBJ *)&gxcoTrivial, &exlo.xlo, &rclDest, &ptlSrc);
            else
                EngCopyBits(psoTarget, psoSource, (CLIPOBJ *)&gxcoTrivial, &exlo.xlo, &rclDest, &ptlSrc);
        }
        else
        {
            LONG cxView = rclView.right - rclView.left, cyView = rclView.bottom - rclView.top;
            LONG cxSource = psoSource->sizlBitmap.cx, cySource = psoSource->sizlBitmap.cy;

            rclDest.left = rclView.left + (LONG)((LONGLONG)rclSrc.left * cxView / cxSource);
            rclDest.top = rclView.top + (LONG)((LONGLONG)rclSrc.top * cyView / cySource);
            rclDest.right = rclView.left + (LONG)(((LONGLONG)rclSrc.right * cxView + cxSource - 1) / cxSource);
            rclDest.bottom = rclView.top + (LONG)(((LONGLONG)rclSrc.bottom * cyView + cySource - 1) / cySource);
            if (rclDest.right <= rclDest.left || rclDest.bottom <= rclDest.top)
                continue;
            rclSrc.left = (LONG)((LONGLONG)(rclDest.left - rclView.left) * cxSource / cxView);
            rclSrc.top = (LONG)((LONGLONG)(rclDest.top - rclView.top) * cySource / cyView);
            rclSrc.right = min((LONG)(((LONGLONG)(rclDest.right - rclView.left) * cxSource + cxView - 1) / cxView), cxSource);
            rclSrc.bottom = min((LONG)(((LONGLONG)(rclDest.bottom - rclView.top) * cySource + cyView - 1) / cyView), cySource);
            if ((psurfTarget->flags & HOOK_STRETCHBLT) && ppdev->pfn.StretchBlt)
            {
                ppdev->pfn.StretchBlt(psoTarget, psoSource, NULL, (CLIPOBJ *)&gxcoTrivial, &exlo.xlo, NULL, NULL,
                                      &rclDest, &rclSrc, NULL, COLORONCOLOR);
            }
            else
            {
                EngStretchBlt(psoTarget, psoSource, NULL, (CLIPOBJ *)&gxcoTrivial, &exlo.xlo, NULL, NULL,
                              &rclDest, &rclSrc, NULL, COLORONCOLOR);
            }
        }
        if (bDirty)
            RECTL_bUnionRect(&rclDirty, &rclDirty, &rclDest);
        else
            rclDirty = rclDest;
        bDirty = TRUE;
    }
    EXLATEOBJ_vCleanup(&exlo);

    if (bDirty && (psurfTarget->flags & HOOK_SYNCHRONIZE) && ppdev->pfn.SynchronizeSurface)
        ppdev->pfn.SynchronizeSurface(psoTarget, &rclDirty, DSS_FLUSH_EVENT);
    EngReleaseSemaphore(ppdev->hsemDevLock);
}

_Requires_lock_held_(gMultiClone.hsem)
static
VOID
MultiFlush(VOID)
{
    PPDEVOBJ ppdevSource = gMultiClone.ppdevSource;
    RECTL arcl[MULTI_MAX_DAMAGE_RECTS];
    ULONG i, cRects;
    BOOL bAnyFill = FALSE;

    if (ppdevSource == NULL || ppdevSource->pSurface == NULL)
        return;

    for (i = 0; i < gMultiClone.cTargets; i++)
        bAnyFill |= gMultiClone.abFill[i];
    if (gMultiClone.bFullDamage || bAnyFill)
    {
        arcl[0].left = arcl[0].top = 0;
        arcl[0].right = ppdevSource->pSurface->SurfObj.sizlBitmap.cx;
        arcl[0].bottom = ppdevSource->pSurface->SurfObj.sizlBitmap.cy;
        cRects = 1;
    }
    else
    {
        cRects = gMultiClone.cDamage;
        RtlCopyMemory(arcl, gMultiClone.arclDamage, cRects * sizeof(RECTL));
    }
    gMultiClone.cDamage = 0;
    gMultiClone.bFullDamage = FALSE;
    if (cRects == 0)
        return;

    for (i = 0; i < gMultiClone.cTargets; i++)
    {
        MultiCopyToTarget(ppdevSource->pSurface, gMultiClone.appdevTarget[i], gMultiClone.abFill[i], arcl, cRects);
        gMultiClone.abFill[i] = FALSE;
    }
}

static
BOOL
APIENTRY
MultiBitBlt(
    SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclTrg,
    POINTL *pptlSrc, POINTL *pptlMask, BRUSHOBJ *pbo, POINTL *pptlBrush, ROP4 rop4)
{
    BOOL bRet = gMultiClone.pfnSource.BitBlt(psoTrg, psoSrc, psoMask, pco, pxlo, prclTrg, pptlSrc, pptlMask,
                                             pbo, pptlBrush, rop4);
    if (bRet)
        MultiAddDamage(psoTrg, pco, prclTrg);
    return bRet;
}

static
BOOL
APIENTRY
MultiCopyBits(
    SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDest, POINTL *pptlSrc)
{
    BOOL bRet = gMultiClone.pfnSource.CopyBits(psoDest, psoSrc, pco, pxlo, prclDest, pptlSrc);
    if (bRet)
        MultiAddDamage(psoDest, pco, prclDest);
    return bRet;
}

static
BOOL
APIENTRY
MultiStretchBlt(
    SURFOBJ *psoDest, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo, COLORADJUSTMENT *pca,
    POINTL *pptlHTOrg, RECTL *prclDest, RECTL *prclSrc, POINTL *pptlMask, ULONG iMode)
{
    BOOL bRet = gMultiClone.pfnSource.StretchBlt(psoDest, psoSrc, psoMask, pco, pxlo, pca, pptlHTOrg,
                                                 prclDest, prclSrc, pptlMask, iMode);
    if (bRet)
        MultiAddDamage(psoDest, pco, prclDest);
    return bRet;
}

static
BOOL
APIENTRY
MultiStretchBltROP(
    SURFOBJ *psoDest, SURFOBJ *psoSrc, SURFOBJ *psoMask, CLIPOBJ *pco, XLATEOBJ *pxlo, COLORADJUSTMENT *pca,
    POINTL *pptlHTOrg, RECTL *prclDest, RECTL *prclSrc, POINTL *pptlMask, ULONG iMode, BRUSHOBJ *pbo, DWORD rop4)
{
    BOOL bRet = gMultiClone.pfnSource.StretchBltROP(psoDest, psoSrc, psoMask, pco, pxlo, pca, pptlHTOrg,
                                                    prclDest, prclSrc, pptlMask, iMode, pbo, rop4);
    if (bRet)
        MultiAddDamage(psoDest, pco, prclDest);
    return bRet;
}

static
BOOL
APIENTRY
MultiTextOut(
    SURFOBJ *pso, STROBJ *pstro, FONTOBJ *pfo, CLIPOBJ *pco, RECTL *prclExtra, RECTL *prclOpaque,
    BRUSHOBJ *pboFore, BRUSHOBJ *pboOpaque, POINTL *pptlOrg, MIX mix)
{
    RECTL rcl = pstro->rclBkGround;
    BOOL bRet = gMultiClone.pfnSource.TextOut(pso, pstro, pfo, pco, prclExtra, prclOpaque, pboFore,
                                              pboOpaque, pptlOrg, mix);
    if (bRet)
    {
        if (prclOpaque)
            RECTL_bUnionRect(&rcl, &rcl, prclOpaque);
        MultiAddDamage(pso, pco, &rcl);
    }
    return bRet;
}

static
BOOL
APIENTRY
MultiLineTo(
    SURFOBJ *pso, CLIPOBJ *pco, BRUSHOBJ *pbo, LONG x1, LONG y1, LONG x2, LONG y2, RECTL *prclBounds, MIX mix)
{
    BOOL bRet = gMultiClone.pfnSource.LineTo(pso, pco, pbo, x1, y1, x2, y2, prclBounds, mix);
    if (bRet)
        MultiAddDamage(pso, pco, prclBounds);
    return bRet;
}

static
BOOL
APIENTRY
MultiPaint(
    SURFOBJ *pso, CLIPOBJ *pco, BRUSHOBJ *pbo, POINTL *pptlBrushOrg, MIX mix)
{
    BOOL bRet = gMultiClone.pfnSource.Paint(pso, pco, pbo, pptlBrushOrg, mix);
    if (bRet)
        MultiAddDamage(pso, pco, NULL);
    return bRet;
}

static
BOOL
APIENTRY
MultiPlgBlt(
    SURFOBJ *psoTrg, SURFOBJ *psoSrc, SURFOBJ *psoMsk, CLIPOBJ *pco, XLATEOBJ *pxlo, COLORADJUSTMENT *pca,
    POINTL *pptlBrushOrg, POINTFIX *pptfx, RECTL *prcl, POINTL *pptl, ULONG iMode)
{
    BOOL bRet = gMultiClone.pfnSource.PlgBlt(psoTrg, psoSrc, psoMsk, pco, pxlo, pca, pptlBrushOrg, pptfx,
                                             prcl, pptl, iMode);
    if (bRet)
        MultiAddDamage(psoTrg, pco, NULL);
    return bRet;
}

static
BOOL
APIENTRY
MultiStrokePath(
    SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, struct _XFORMOBJ *pxo, BRUSHOBJ *pbo, POINTL *pptlBrushOrg,
    LINEATTRS *plineattrs, MIX mix)
{
    BOOL bRet = gMultiClone.pfnSource.StrokePath(pso, ppo, pco, pxo, pbo, pptlBrushOrg, plineattrs, mix);
    if (bRet)
        MultiAddDamage(pso, pco, NULL);
    return bRet;
}

static
BOOL
APIENTRY
MultiFillPath(
    SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, BRUSHOBJ *pbo, POINTL *pptlBrushOrg, MIX mix, FLONG flOptions)
{
    BOOL bRet = gMultiClone.pfnSource.FillPath(pso, ppo, pco, pbo, pptlBrushOrg, mix, flOptions);
    if (bRet)
        MultiAddDamage(pso, pco, NULL);
    return bRet;
}

static
BOOL
APIENTRY
MultiStrokeAndFillPath(
    SURFOBJ *pso, PATHOBJ *ppo, CLIPOBJ *pco, struct _XFORMOBJ *pxo, BRUSHOBJ *pboStroke, LINEATTRS *plineattrs,
    BRUSHOBJ *pboFill, POINTL *pptlBrushOrg, MIX mixFill, FLONG flOptions)
{
    BOOL bRet = gMultiClone.pfnSource.StrokeAndFillPath(pso, ppo, pco, pxo, pboStroke, plineattrs, pboFill,
                                                        pptlBrushOrg, mixFill, flOptions);
    if (bRet)
        MultiAddDamage(pso, pco, NULL);
    return bRet;
}

static
BOOL
APIENTRY
MultiAlphaBlend(
    SURFOBJ *psoDest, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDest, RECTL *prclSrc,
    BLENDOBJ *pBlendObj)
{
    BOOL bRet = gMultiClone.pfnSource.AlphaBlend(psoDest, psoSrc, pco, pxlo, prclDest, prclSrc, pBlendObj);
    if (bRet)
        MultiAddDamage(psoDest, pco, prclDest);
    return bRet;
}

static
BOOL
APIENTRY
MultiTransparentBlt(
    SURFOBJ *psoDst, SURFOBJ *psoSrc, CLIPOBJ *pco, XLATEOBJ *pxlo, RECTL *prclDst, RECTL *prclSrc,
    ULONG iTransColor, ULONG ulReserved)
{
    BOOL bRet = gMultiClone.pfnSource.TransparentBlt(psoDst, psoSrc, pco, pxlo, prclDst, prclSrc,
                                                     iTransColor, ulReserved);
    if (bRet)
        MultiAddDamage(psoDst, pco, prclDst);
    return bRet;
}

static
BOOL
APIENTRY
MultiGradientFill(
    SURFOBJ *psoDest, CLIPOBJ *pco, XLATEOBJ *pxlo, TRIVERTEX *pVertex, ULONG nVertex, PVOID pMesh,
    ULONG nMesh, RECTL *prclExtents, POINTL *pptlDitherOrg, ULONG ulMode)
{
    BOOL bRet = gMultiClone.pfnSource.GradientFill(psoDest, pco, pxlo, pVertex, nVertex, pMesh, nMesh,
                                                   prclExtents, pptlDitherOrg, ulMode);
    if (bRet)
        MultiAddDamage(psoDest, pco, prclExtents);
    return bRet;
}

static
VOID
APIENTRY
MultiSynchronizeSurface(
    SURFOBJ *pso, RECTL *prcl, FLONG fl)
{
    if (gMultiClone.pfnSource.SynchronizeSurface)
        gMultiClone.pfnSource.SynchronizeSurface(pso, prcl, fl);

    if (!MultiIsSourcePrimary(pso) || !(fl & (DSS_TIMER_EVENT | DSS_FLUSH_EVENT)))
        return;
    if (prcl)
        MultiAddDamage(pso, NULL, prcl);

    EngAcquireSemaphore(gMultiClone.hsem);
    MultiFlush();
    EngReleaseSemaphore(gMultiClone.hsem);
}

static const struct
{
    ULONG iFunc;
    PFN pfn;
} gMultiCloneHooks[] =
{
    { INDEX_DrvBitBlt, (PFN)MultiBitBlt },
    { INDEX_DrvCopyBits, (PFN)MultiCopyBits },
    { INDEX_DrvStretchBlt, (PFN)MultiStretchBlt },
    { INDEX_DrvStretchBltROP, (PFN)MultiStretchBltROP },
    { INDEX_DrvTextOut, (PFN)MultiTextOut },
    { INDEX_DrvLineTo, (PFN)MultiLineTo },
    { INDEX_DrvPaint, (PFN)MultiPaint },
    { INDEX_DrvPlgBlt, (PFN)MultiPlgBlt },
    { INDEX_DrvStrokePath, (PFN)MultiStrokePath },
    { INDEX_DrvFillPath, (PFN)MultiFillPath },
    { INDEX_DrvStrokeAndFillPath, (PFN)MultiStrokeAndFillPath },
    { INDEX_DrvAlphaBlend, (PFN)MultiAlphaBlend },
    { INDEX_DrvTransparentBlt, (PFN)MultiTransparentBlt },
    { INDEX_DrvGradientFill, (PFN)MultiGradientFill },
    { INDEX_DrvSynchronizeSurface, (PFN)MultiSynchronizeSurface },
};

_Requires_lock_held_(gMultiClone.hsem)
static
VOID
MultiHookSource(
    _Inout_ PPDEVOBJ ppdev)
{
    ULONG i;

    /* TODO: forward the pointer shape and position to the clone targets */
    gMultiClone.pfnSource = ppdev->pfn;
    for (i = 0; i < RTL_NUMBER_OF(gMultiCloneHooks); i++)
    {
        if (ppdev->apfn[gMultiCloneHooks[i].iFunc] != NULL)
            ppdev->apfn[gMultiCloneHooks[i].iFunc] = gMultiCloneHooks[i].pfn;
    }
}

_Requires_lock_held_(gMultiClone.hsem)
static
VOID
MultiUnhookSource(
    _Inout_ PPDEVOBJ ppdev)
{
    PVOID *apfnSource = (PVOID *)&gMultiClone.pfnSource;
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(gMultiCloneHooks); i++)
    {
        if (ppdev->apfn[gMultiCloneHooks[i].iFunc] == (PVOID)gMultiCloneHooks[i].pfn)
            ppdev->apfn[gMultiCloneHooks[i].iFunc] = apfnSource[gMultiCloneHooks[i].iFunc];
    }
}

BOOL
MultiCloneAttach(
    _Inout_ PPDEVOBJ ppdevSource,
    _Inout_ PPDEVOBJ ppdevTarget)
{
    BOOL bRet = FALSE;

    EngAcquireSemaphore(ppdevSource->hsemDevLock);
    EngAcquireSemaphore(gMultiClone.hsem);
    if ((gMultiClone.ppdevSource == NULL || gMultiClone.ppdevSource == ppdevSource) &&
        gMultiClone.cTargets < MULTI_MAX_CLONE_TARGETS &&
        ppdevTarget != ppdevSource)
    {
        if (gMultiClone.cTargets == 0)
        {
            MultiHookSource(ppdevSource);
            gMultiClone.ppdevSource = ppdevSource;
        }
        ppdevTarget->flFlags |= PDEV_CLONE_DEVICE;
        ppdevTarget->ppdevParent = ppdevSource;
        gMultiClone.abFill[gMultiClone.cTargets] = TRUE;
        gMultiClone.appdevTarget[gMultiClone.cTargets++] = ppdevTarget;
        MultiFlush();
        bRet = TRUE;
    }
    EngReleaseSemaphore(gMultiClone.hsem);
    EngReleaseSemaphore(ppdevSource->hsemDevLock);
    return bRet;
}

PPDEVOBJ
MultiCloneDetach(
    _In_opt_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    PPDEVOBJ ppdevSource, ppdevTarget = NULL;
    ULONG i;

    EngAcquireSemaphore(gMultiClone.hsem);
    ppdevSource = gMultiClone.ppdevSource;
    if (ppdevSource)
        PDEVOBJ_vReference(ppdevSource);
    EngReleaseSemaphore(gMultiClone.hsem);
    if (!ppdevSource)
        return NULL;

    EngAcquireSemaphore(ppdevSource->hsemDevLock);
    EngAcquireSemaphore(gMultiClone.hsem);
    for (i = 0; i < gMultiClone.cTargets; i++)
    {
        if (!pGraphicsDevice || gMultiClone.appdevTarget[i]->pGraphicsDevice == pGraphicsDevice)
        {
            ppdevTarget = gMultiClone.appdevTarget[i];
            gMultiClone.cTargets--;
            gMultiClone.appdevTarget[i] = gMultiClone.appdevTarget[gMultiClone.cTargets];
            gMultiClone.abFill[i] = gMultiClone.abFill[gMultiClone.cTargets];
            break;
        }
    }
    if (ppdevTarget && gMultiClone.cTargets == 0 && gMultiClone.ppdevSource == ppdevSource)
    {
        MultiUnhookSource(ppdevSource);
        gMultiClone.ppdevSource = NULL;
        gMultiClone.cDamage = 0;
        gMultiClone.bFullDamage = FALSE;
    }
    EngReleaseSemaphore(gMultiClone.hsem);
    EngReleaseSemaphore(ppdevSource->hsemDevLock);
    PDEVOBJ_vRelease(ppdevSource);

    if (ppdevTarget)
    {
        ppdevTarget->flFlags &= ~PDEV_CLONE_DEVICE;
        ppdevTarget->ppdevParent = NULL;
    }
    return ppdevTarget;
}

BOOL
MultiCloneIsTarget(
    _In_ PGRAPHICS_DEVICE pGraphicsDevice)
{
    BOOL bFound = FALSE;
    ULONG i;

    EngAcquireSemaphoreShared(gMultiClone.hsem);
    for (i = 0; i < gMultiClone.cTargets && !bFound; i++)
        bFound = gMultiClone.appdevTarget[i]->pGraphicsDevice == pGraphicsDevice;
    EngReleaseSemaphore(gMultiClone.hsem);
    return bFound;
}

ULONG
MultiCloneGetTargets(
    _In_ PPDEVOBJ ppdevSource,
    _Out_writes_(cMax) PGRAPHICS_DEVICE *apGraphicsDevice,
    _In_ ULONG cMax)
{
    ULONG i, c = 0;

    EngAcquireSemaphoreShared(gMultiClone.hsem);
    if (gMultiClone.ppdevSource == ppdevSource)
    {
        for (i = 0; i < gMultiClone.cTargets && c < cMax; i++)
            apGraphicsDevice[c++] = gMultiClone.appdevTarget[i]->pGraphicsDevice;
    }
    EngReleaseSemaphore(gMultiClone.hsem);
    return c;
}

_Requires_lock_held_(ppdev->hsemDevLock)
VOID
MultiCloneSuspend(
    _Inout_ PPDEVOBJ ppdev)
{
    EngAcquireSemaphore(gMultiClone.hsem);
    if (gMultiClone.ppdevSource == ppdev)
        MultiUnhookSource(ppdev);
    EngReleaseSemaphore(gMultiClone.hsem);
}

_Requires_lock_held_(ppdev->hsemDevLock)
VOID
MultiCloneResume(
    _Inout_ PPDEVOBJ ppdev)
{
    ULONG i;

    EngAcquireSemaphore(gMultiClone.hsem);
    if (gMultiClone.ppdevSource == ppdev)
    {
        MultiHookSource(ppdev);
        for (i = 0; i < gMultiClone.cTargets; i++)
            gMultiClone.abFill[i] = TRUE;
        MultiFlush();
    }
    EngReleaseSemaphore(gMultiClone.hsem);
}
