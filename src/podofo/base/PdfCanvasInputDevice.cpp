/**
 * Copyright (C) 2021 by Francesco Pretto <ceztko@gmail.com>
 *
 * Licensed under GNU Lesser General Public License 2.1 or later.
 * Some rights reserved. See COPYING, AUTHORS.
 */

#include "PdfDefinesPrivate.h"
#include "PdfCanvasInputDevice.h"
#include "PdfCanvas.h"

using namespace std;
using namespace PoDoFo;

PdfCanvasInputDevice::PdfCanvasInputDevice(PdfCanvas& canvas)
{
    PdfObject& contents = canvas.GetContents();
    if (contents.IsArray())
    {
        PdfArray& contentsArr = contents.GetArray();
        for (unsigned i = 0; i < contentsArr.GetSize(); i++)
        {
            auto& streamObj = contentsArr.FindAt(i);
            m_lstContents.push_back(&streamObj);
        }
    }
    else if (contents.IsDictionary())
    {
        // NOTE: Pages are allowed to be empty
        if (contents.HasStream())
            m_lstContents.push_back(&contents);
    }
    else
    {
        PODOFO_RAISE_ERROR_INFO(EPdfError::InvalidDataType, "Page /Contents not stream or array of streams");
    }

    if (m_lstContents.size() == 0)
    {
        m_eof = true;
    }
    else
    {
        popNextDevice();
        m_eof = m_device->Eof();
    }
}

bool PdfCanvasInputDevice::TryGetChar(char& ch)
{
    if (m_eof)
    {
        ch = 0;
        return false;
    }

    PdfInputDevice* device = nullptr;
    while (true)
    {
        if (!tryGetNextDevice(device))
        {
            m_eof = true;
            return false;
        }

        if (device->TryGetChar(ch))
            return true;
    }
}

int PdfCanvasInputDevice::Look()
{
    if (m_eof)
        return EOF;

    PdfInputDevice* device = nullptr;
    while (true)
    {
        if (!tryGetNextDevice(device))
        {
            m_eof = true;
            return EOF;
        }

        int ret = device->Look();
        if (ret != EOF)
            return ret;
    }
}

size_t PdfCanvasInputDevice::Read(char* buffer, size_t size)
{
    if (size == 0 || m_eof)
        return 0;

    size_t readCount = 0;
    PdfInputDevice* device = nullptr;
    while (true)
    {
        if (!tryGetNextDevice(device))
        {
            m_eof = true;
            return readCount;
        }

        // Span reads into multple input devices
        size_t readLocal = device->Read(buffer + readCount, size);
        size -= readLocal;
        readCount += readLocal;

        if (size == 0)
            return readCount;
    }
}

size_t PdfCanvasInputDevice::Tell()
{
    throw runtime_error("Unsupported");
}

bool PdfCanvasInputDevice::tryGetNextDevice(PdfInputDevice*& device)
{
    PODOFO_ASSERT(m_device != nullptr);
    if (device == nullptr)
    {
        device = m_device.get();
        return true;
    }

    if (m_lstContents.size() == 0)
    {
        device = nullptr;
        return false;
    }

    popNextDevice();
    device = m_device.get();
    return true;
}

void PdfCanvasInputDevice::popNextDevice()
{
    m_device = std::make_unique<PdfInputDevice>(m_lstContents.front()->GetOrCreateStream());
    m_lstContents.pop_front();
}