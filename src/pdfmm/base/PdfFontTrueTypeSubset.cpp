/**
 * Copyright (C) 2008 by Dominik Seichter <domseichter@web.de>
 * Copyright (C) 2021 by Francesco Pretto <ceztko@gmail.com>
 *
 * Licensed under GNU Library General Public License 2.0 or later.
 * Some rights reserved. See COPYING, AUTHORS.
 */

// The code was initally based on work by ZhangYang
// (张杨.国际) <zhang_yang@founder.com>

#include <pdfmm/private/PdfDeclarationsPrivate.h>
#include "PdfFontTrueTypeSubset.h"

#include <pdfmm/private/FreetypePrivate.h>
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <algorithm>

#include "PdfInputDevice.h"
#include "PdfOutputDevice.h"

using namespace std;
using namespace mm;

// Required TrueType tables
enum class ReqTable
{
    none = 0,
    head = 1,
    hhea = 2,
    loca = 4,
    maxp = 8,
    glyf = 16,
    hmtx = 32,
    all = head | hhea | loca | maxp | glyf | hmtx,
};

ENABLE_BITMASK_OPERATORS(ReqTable);

static constexpr unsigned LENGTH_HEADER12 = 12;
static constexpr unsigned LENGTH_OFFSETTABLE16 = 16;

//Get the number of bytes to pad the ul, because of 4-byte-alignment.
static uint32_t GetTableCheksum(const char* buf, uint32_t size);

static bool TryAdvanceCompoundOffset(unsigned& offset, unsigned flags);

PdfFontTrueTypeSubset::PdfFontTrueTypeSubset(PdfInputDevice& device) :
    m_device(&device),
    m_isLongLoca(false),
    m_glyphCount(0),
    m_HMetricsCount(0)
{
}

void PdfFontTrueTypeSubset::BuildFont(std::string& output, const PdfFontMetrics& metrics,
    const GIDList& gidList)
{
    switch (metrics.GetFontFileType())
    {
        case PdfFontFileType::TrueType:
        case PdfFontFileType::OpenType:
            break;
        default:
            PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InvalidFontFile, "The font to be subsetted is not a TrueType font");
    }

    PdfMemoryInputDevice input(metrics.GetFontFileData());
    PdfFontTrueTypeSubset subset(input);
    subset.BuildFont(output, gidList);
}

void PdfFontTrueTypeSubset::BuildFont(string& buffer, const GIDList& gidList)
{
    Init();

    GlyphContext context;
    context.GlyfTableOffset = GetTableOffset(TTAG_glyf);
    context.LocaTableOffset = GetTableOffset(TTAG_loca);
    LoadGlyphs(context, gidList);
    WriteTables(buffer);
}

void PdfFontTrueTypeSubset::Init()
{
    InitTables();
    GetNumberOfGlyphs();
    SeeIfLongLocaOrNot();
}

unsigned PdfFontTrueTypeSubset::GetTableOffset(unsigned tag)
{
    for (auto& table : m_tables)
    {
        if (table.Tag == tag)
            return table.Offset;
    }
    PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "table missing");
}

void PdfFontTrueTypeSubset::GetNumberOfGlyphs()
{
    unsigned offset = GetTableOffset(TTAG_maxp);

    m_device->Seek(offset + sizeof(uint32_t) * 1);
    utls::ReadUInt16BE(*m_device, m_glyphCount);

    offset = GetTableOffset(TTAG_hhea);

    m_device->Seek(offset + sizeof(uint16_t) * 17);
    utls::ReadUInt16BE(*m_device, m_HMetricsCount);
}

void PdfFontTrueTypeSubset::InitTables()
{
    uint16_t tableCount;
    m_device->Seek(sizeof(uint32_t) * 1);
    utls::ReadUInt16BE(*m_device, tableCount);

    ReqTable tableMask = ReqTable::none;
    TrueTypeTable tbl;

    for (unsigned short i = 0; i < tableCount; i++)
    {
        // Name of each table:
        m_device->Seek(LENGTH_HEADER12 + LENGTH_OFFSETTABLE16 * i);
        utls::ReadUInt32BE(*m_device, tbl.Tag);

        // Checksum of each table:
        m_device->Seek(LENGTH_HEADER12 + LENGTH_OFFSETTABLE16 * i + sizeof(uint32_t) * 1);
        utls::ReadUInt32BE(*m_device, tbl.Checksum);

        // Offset of each table:
        m_device->Seek(LENGTH_HEADER12 + LENGTH_OFFSETTABLE16 * i + sizeof(uint32_t) * 2);
        utls::ReadUInt32BE(*m_device, tbl.Offset);

        // Length of each table:
        m_device->Seek(LENGTH_HEADER12 + LENGTH_OFFSETTABLE16 * i + sizeof(uint32_t) * 3);
        utls::ReadUInt32BE(*m_device, tbl.Length);

        // PDF 32000-1:2008 9.9 Embedded Font Programs
        // "These TrueType tables shall always be present if present in the original TrueType font program:
        // 'head', 'hhea', 'loca', 'maxp', 'cvt','prep', 'glyf', 'hmtx' and 'fpgm'. [..]  If used with a
        // CIDFont dictionary, the 'cmap' table is not needed and shall not be present

        bool skipTable = false;
        switch (tbl.Tag)
        {
            case TTAG_head:
                tableMask |= ReqTable::head;
                break;
            case TTAG_hhea:
                // required to get numHMetrics
                tableMask |= ReqTable::hhea;
                break;
            case TTAG_loca:
                tableMask |= ReqTable::loca;
                break;
            case TTAG_maxp:
                tableMask |= ReqTable::maxp;
                break;
            case TTAG_glyf:
                tableMask |= ReqTable::glyf;
                break;
            case TTAG_hmtx:
                // advance width
                tableMask |= ReqTable::hmtx;
                break;
            case TTAG_cvt:
            case TTAG_fpgm:
            case TTAG_prep:
                // Just include these tables inconditionally if present
                // in the original font
                break;
            case TTAG_post:
                if (tbl.Length < 32)
                    skipTable = true;

                // Reduce table size, leter we will change format to 'post' Format 3
                tbl.Length = 32;
                break;
            // Exclude all other tables, including cmap which
            // is not required
            case TTAG_cmap:
            default:
                skipTable = true;
                break;
        }
        if (!skipTable)
            m_tables.push_back(tbl);
    }

    if ((tableMask & ReqTable::all) == ReqTable::none)
        PDFMM_RAISE_ERROR_INFO(PdfErrorCode::UnsupportedFontFormat, "Required TrueType table missing");
}

void PdfFontTrueTypeSubset::SeeIfLongLocaOrNot()
{
    unsigned headOffset = GetTableOffset(TTAG_head);
    uint16_t isLong;
    m_device->Seek(headOffset + 50);
    utls::ReadUInt16BE(*m_device, isLong);
    m_isLongLoca = (isLong == 0 ? false : true);  // 1 for long
}

void PdfFontTrueTypeSubset::LoadGlyphs(GlyphContext& ctx, const GIDList& gidList)
{
    // For any fonts, assume that glyph 0 is needed.
    LoadGID(ctx, 0);
    for (unsigned gid : gidList)
        LoadGID(ctx, gid);

    // Map original GIDs to a new index as they will appear in the subset
    map<unsigned, unsigned> glyphIndexMap;
    glyphIndexMap.insert({ 0, 0 });
    m_orderedGIDs.push_back(0);
    for (unsigned gid : gidList)
    {
        glyphIndexMap.insert({ gid, (unsigned)glyphIndexMap.size() });
        m_orderedGIDs.push_back(gid);
    }

    for (auto& pair : m_glyphDatas)
    {
        auto& glyphData = pair.second;
        if (!glyphData.IsCompound)
            continue;

        GlyphCompoundData cmpData;
        unsigned offset = 0;
        while (true)
        {
            unsigned componentGlyphIdOffset = glyphData.GlyphAdvOffset + offset;
            ReadGlyphCompoundData(cmpData, componentGlyphIdOffset);
            // Try remap the GID
            auto inserted = glyphIndexMap.insert({ cmpData.GlyphIndex, (unsigned)glyphIndexMap.size() });
            if (inserted.second)
            {
                // If insertion occurred, insert it to the original GIDs ordered list
                m_orderedGIDs.push_back(cmpData.GlyphIndex);
            }

            // Insert the compound component using the actual assigned GID
            glyphData.CompoundComponents.push_back(
                { (unsigned)(componentGlyphIdOffset + sizeof(uint16_t)) - glyphData.GlyphOffset, inserted.first->second });
            if (!TryAdvanceCompoundOffset(offset, cmpData.Flags))
                break;
        }
    }
}

void PdfFontTrueTypeSubset::LoadGID(GlyphContext& ctx, unsigned gid)
{
    if (gid >= m_glyphCount)
        PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "GID out of range");

    if (m_glyphDatas.find(gid) != m_glyphDatas.end())
        return;

    // https://docs.microsoft.com/en-us/typography/opentype/spec/loca
    auto& glyphData = m_glyphDatas[gid] = { };
    if (m_isLongLoca)
    {
        uint32_t offset1;
        uint32_t offset2;
        m_device->Seek(ctx.LocaTableOffset + sizeof(uint32_t) * gid);
        utls::ReadUInt32BE(*m_device, offset1);

        m_device->Seek(ctx.LocaTableOffset + sizeof(uint32_t) * (gid + 1));
        utls::ReadUInt32BE(*m_device, offset2);

        glyphData.GlyphLength = offset2 - offset1;
        glyphData.GlyphOffset = ctx.GlyfTableOffset + offset1;
    }
    else
    {
        uint16_t offset1;
        uint16_t offset2;

        m_device->Seek(ctx.LocaTableOffset + sizeof(uint16_t) * gid);
        utls::ReadUInt16BE(*m_device, offset1);
        unsigned uoffset1 = (unsigned)offset1 << 1u; // Handle the possible overflow

        m_device->Seek(ctx.LocaTableOffset + sizeof(uint16_t) * (gid + 1));
        utls::ReadUInt16BE(*m_device, offset2);
        unsigned uoffset2 = (unsigned)offset2 << 1u; // Handle the possible overflow

        glyphData.GlyphLength = uoffset2 - uoffset1;
        glyphData.GlyphOffset = ctx.GlyfTableOffset + uoffset1;
    }

    glyphData.GlyphAdvOffset = glyphData.GlyphOffset + 5 * sizeof(uint16_t);

    m_device->Seek(glyphData.GlyphOffset);
    utls::ReadInt16BE(*m_device, ctx.ContourCount);
    if (ctx.ContourCount < 0)
    {
        glyphData.IsCompound = true;
        LoadCompound(ctx, glyphData);
    }
}

void PdfFontTrueTypeSubset::LoadCompound(GlyphContext& ctx, const GlyphData& data)
{
    GlyphCompoundData cmpData;
    unsigned offset = 0;
    while (true)
    {
        ReadGlyphCompoundData(cmpData, data.GlyphAdvOffset + offset);
        LoadGID(ctx, cmpData.GlyphIndex);
        if (!TryAdvanceCompoundOffset(offset, cmpData.Flags))
            break;
    }
}

// Ref: https://docs.microsoft.com/en-us/typography/opentype/spec/glyf
void PdfFontTrueTypeSubset::WriteGlyphTable(PdfOutputDevice& output)
{
    for (unsigned gid : m_orderedGIDs)
    {
        auto& glyphData = m_glyphDatas[gid];
        if (glyphData.GlyphLength == 0)
            continue;

        if (glyphData.IsCompound)
        {
            // Fix the compound glyph data to remap original GIDs indices
            // as they will appear in the subset

            m_tmpBuffer.resize(glyphData.GlyphLength);
            m_device->Seek(glyphData.GlyphOffset);
            m_device->Read(m_tmpBuffer.data(), glyphData.GlyphLength);
            for (auto& component : glyphData.CompoundComponents)
                utls::WriteUInt16BE(m_tmpBuffer.data() + component.Offset, (uint16_t)component.GlyphIndex);
            output.Write(m_tmpBuffer);
        }
        else
        {
            // The simple glyph data doesn't need to be fixed
            CopyData(output, glyphData.GlyphOffset, glyphData.GlyphLength);
        }
    }
}

// The 'hmtx' table contains the horizontal metrics for each glyph in the font
// https://docs.microsoft.com/en-us/typography/opentype/spec/hmtx
void PdfFontTrueTypeSubset::WriteHmtxTable(PdfOutputDevice& output)
{
    struct LongHorMetrics
    {
        uint16_t AdvanceWidth;
        int16_t LeftSideBearing;
    };

    unsigned tableOffset = GetTableOffset(TTAG_hmtx);
    for (unsigned gid : m_orderedGIDs)
        CopyData(output, tableOffset + gid * sizeof(LongHorMetrics), sizeof(LongHorMetrics));
}

// "The 'loca' table stores the offsets to the locations
// of the glyphs in the font relative to the beginning of
// the 'glyf' table. [..] To make it possible to compute
// the length of the last glyph element, there is an extra
// entry after the offset that points to the last valid
// index. This index points to the end of the glyph data"
// Ref: https://docs.microsoft.com/en-us/typography/opentype/spec/loca
void PdfFontTrueTypeSubset::WriteLocaTable(PdfOutputDevice& output)
{
    uint32_t glyphAddress = 0;
    if (m_isLongLoca)
    {
        for (unsigned gid : m_orderedGIDs)
        {
            auto& glyphData = m_glyphDatas[gid];
            utls::WriteUInt32BE(output, glyphAddress);
            glyphAddress += glyphData.GlyphLength;
        }

        // Last "extra" entry
        utls::WriteUInt32BE(output, glyphAddress);
    }
    else
    {
        for (unsigned gid : m_orderedGIDs)
        {
            auto& glyphData = m_glyphDatas[gid];
            utls::WriteUInt16BE(output, static_cast<uint16_t>(glyphAddress >> 1));
            glyphAddress += glyphData.GlyphLength;
        }

        // Last "extra" entry
        utls::WriteUInt16BE(output, static_cast<uint16_t>(glyphAddress >> 1));
    }
}

void PdfFontTrueTypeSubset::WriteTables(string& buffer)
{
    PdfStringOutputDevice output(buffer);

    uint16_t entrySelector = (uint16_t)std::ceil(std::log2(m_tables.size()));
    uint16_t searchRange = (uint16_t)std::pow(2, entrySelector);
    uint16_t rangeShift = (16 * (uint16_t)m_tables.size()) - searchRange;

    // Write the font directory table
    // https://docs.microsoft.com/en-us/typography/opentype/spec/otff#tabledirectory
    utls::WriteUInt32BE(output, 0x00010000);     // Scaler type, 0x00010000 is True type font
    utls::WriteUInt16BE(output, (uint16_t)m_tables.size());
    utls::WriteUInt16BE(output, searchRange);
    utls::WriteUInt16BE(output, entrySelector);
    utls::WriteUInt16BE(output, rangeShift);

    size_t directoryTableOffset = output.Tell();

    // Prepare table offsets
    for (unsigned i = 0; i < m_tables.size(); i++)
    {
        auto& table = m_tables[i];
        utls::WriteUInt32BE(output, table.Tag);
        // Write empty placeholders
        utls::WriteUInt32BE(output, 0); // Table checksum
        utls::WriteUInt32BE(output, 0); // Table offset
        utls::WriteUInt32BE(output, 0); // Table length (actual length not padded length)
    }

    nullable<size_t> headOffset;
    size_t tableOffset;
    for (unsigned i = 0; i < m_tables.size(); i++)
    {
        auto& table = m_tables[i];
        tableOffset = output.Tell();
        switch (table.Tag)
        {
            case TTAG_head:
                headOffset = tableOffset;
                CopyData(output, table.Offset, table.Length);
                // Set the checkSumAdjustment to 0
                utls::WriteUInt32BE(buffer.data() + tableOffset + 4, 0);
                break;
            case TTAG_maxp:
                // https://docs.microsoft.com/en-us/typography/opentype/spec/maxp
                CopyData(output, table.Offset, table.Length);
                // Write the number of glyphs in the font
                utls::WriteUInt16BE(buffer.data() + tableOffset + 4, (uint16_t)m_glyphDatas.size());
                break;
            case TTAG_hhea:
                // https://docs.microsoft.com/en-us/typography/opentype/spec/hhea
                CopyData(output, table.Offset, table.Length);
                // Write numOfLongHorMetrics, see also 'hmtx' table
                utls::WriteUInt16BE(buffer.data() + tableOffset + 34, (uint16_t)m_glyphDatas.size());
                break;
            case TTAG_post:
                // https://docs.microsoft.com/en-us/typography/opentype/spec/post
                CopyData(output, table.Offset, table.Length);
                // Enforce 'post' Format 3, written as a Fixed 16.16 number
                utls::WriteUInt32BE(buffer.data() + tableOffset, 0x00030000);
                // Clear Type42/Type1 font information
                memset(buffer.data() + tableOffset + 16, 0, 16);
                break;
            case TTAG_glyf:
                WriteGlyphTable(output);
                break;
            case TTAG_loca:
                WriteLocaTable(output);
                break;
            case TTAG_hmtx:
                WriteHmtxTable(output);
                break;
            case TTAG_cvt:
            case TTAG_fpgm:
            case TTAG_prep:
                CopyData(output, table.Offset, table.Length);
                break;
            default:
                PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InvalidEnumValue, "Unsupported table at this context");
        }

        // Align the table length to 4 bytes and pad remaing space with zeroes
        size_t tableLength = output.Tell() - tableOffset;
        size_t tableLengthPadded = (tableLength + 3) & ~3;
        for (size_t i = tableLength; i < tableLengthPadded; i++)
            output.Put('\0');

        // Write dynamic font directory table entries
        size_t currDirTableOffset = directoryTableOffset + i * LENGTH_OFFSETTABLE16;
        utls::WriteUInt32BE(buffer.data() + currDirTableOffset + 4, GetTableCheksum(buffer.data() + tableOffset, (uint32_t)tableLength));
        utls::WriteUInt32BE(buffer.data() + currDirTableOffset + 8, (uint32_t)tableOffset);
        utls::WriteUInt32BE(buffer.data() + currDirTableOffset + 12, (uint32_t)tableLength);
    }

    // Check for head table
    if (!headOffset.has_value())
        PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "'head' table missing");

    // As explained in the "Table Directory"
    // https://docs.microsoft.com/en-us/typography/opentype/spec/otff#tabledirectory
    uint32_t fontChecksum = 0xB1B0AFBA - GetTableCheksum(buffer.data(), (uint32_t)output.Tell());
    utls::WriteUInt32BE(buffer.data() + *headOffset + 4, fontChecksum);
}

void PdfFontTrueTypeSubset::ReadGlyphCompoundData(GlyphCompoundData& data, unsigned offset)
{
    uint16_t temp;
    m_device->Seek(offset);
    utls::ReadUInt16BE(*m_device, temp);
    data.Flags = temp;

    m_device->Seek(offset + sizeof(uint16_t));
    utls::ReadUInt16BE(*m_device, temp);
    data.GlyphIndex = temp;
}

bool TryAdvanceCompoundOffset(unsigned& offset, unsigned flags)
{
    constexpr unsigned ARG_1_AND_2_ARE_WORDS = 0x01;
    constexpr unsigned WE_HAVE_A_SCALE = 0x08;
    constexpr unsigned MORE_COMPONENTS = 0x20;
    constexpr unsigned WE_HAVE_AN_X_AND_Y_SCALE = 0x40;
    constexpr unsigned WE_HAVE_TWO_BY_TWO = 0x80;

    if ((flags & MORE_COMPONENTS) == 0)
        return false;

    offset += (flags & ARG_1_AND_2_ARE_WORDS) ? 4 * sizeof(uint16_t) : 3 * sizeof(uint16_t);
    if ((flags & WE_HAVE_A_SCALE) != 0)
        offset += sizeof(uint16_t);
    else if ((flags & WE_HAVE_AN_X_AND_Y_SCALE) != 0)
        offset += 2 * sizeof(uint16_t);
    else if ((flags & WE_HAVE_TWO_BY_TWO) != 0)
        offset += 4 * sizeof(uint16_t);

    return true;
}

void PdfFontTrueTypeSubset::CopyData(PdfOutputDevice& output, unsigned offset, unsigned size)
{
    m_device->Seek(offset);
    m_tmpBuffer.resize(size);
    m_device->Read(m_tmpBuffer.data(), size);
    output.Write(m_tmpBuffer);
}

uint32_t GetTableCheksum(const char* buf, uint32_t size)
{
    // As explained in the "Table Directory"
    // https://docs.microsoft.com/en-us/typography/opentype/spec/otff#tabledirectory
    uint32_t sum = 0;
    uint32_t nLongs = (size + 3) / 4;
    while (nLongs-- > 0)
        sum += *buf++;

    return sum;
}
