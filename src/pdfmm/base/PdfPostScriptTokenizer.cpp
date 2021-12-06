/**
 * Copyright (C) 2021 by Francesco Pretto <ceztko@gmail.com>
 *
 * Licensed under GNU Lesser General Public License 2.1.
 * Some rights reserved. See COPYING, AUTHORS.
 */

#include <pdfmm/private/PdfDeclarationsPrivate.h>
#include "PdfPostScriptTokenizer.h"

using namespace std;
using namespace mm;

PdfPostScriptTokenizer::PdfPostScriptTokenizer()
    : PdfTokenizer(false) { }

PdfPostScriptTokenizer::PdfPostScriptTokenizer(const shared_ptr<chars>& buffer)
    : PdfTokenizer(buffer, false) { }

void PdfPostScriptTokenizer::ReadNextVariant(PdfInputDevice& device, PdfVariant& variant)
{
    if (!TryReadNextVariant(device, variant))
        PDFMM_RAISE_ERROR_INFO(PdfErrorCode::UnexpectedEOF, "Expected variant");
}

bool PdfPostScriptTokenizer::TryReadNextVariant(PdfInputDevice& device, PdfVariant& variant)
{
    PdfTokenType tokenType;
    string_view token;
    if (!PdfTokenizer::TryReadNextToken(device, token, tokenType))
        return false;

    return PdfTokenizer::TryReadNextVariant(device, token, tokenType, variant, nullptr);
}

bool PdfPostScriptTokenizer::TryReadNext(PdfInputDevice& device, PdfPostScriptTokenType& psTokenType, string_view& keyword, PdfVariant& variant)
{
    PdfTokenType tokenType;
    string_view token;
    keyword = { };
    bool gotToken = PdfTokenizer::TryReadNextToken(device, token, tokenType);
    if (!gotToken)
    {
        psTokenType = PdfPostScriptTokenType::Unknown;
        return false;
    }

    // Try first to detect PS procedures delimiters
    switch (tokenType)
    {
        case PdfTokenType::BraceLeft:
            psTokenType = PdfPostScriptTokenType::ProcedureEnter;
            return true;
        case PdfTokenType::BraceRight:
            psTokenType = PdfPostScriptTokenType::ProcedureExit;
            return true;
        default:
            // Continue evaluating data type
            break;
    }

    PdfLiteralDataType dataType = DetermineDataType(device, token, tokenType, variant);

    // asume we read a variant unless we discover otherwise later.
    psTokenType = PdfPostScriptTokenType::Variant;
    switch (dataType)
    {
        case PdfLiteralDataType::Null:
        case PdfLiteralDataType::Bool:
        case PdfLiteralDataType::Number:
        case PdfLiteralDataType::Real:
            // the data was already read into variant by the DetermineDataType function
            break;

        case PdfLiteralDataType::Dictionary:
            this->ReadDictionary(device, variant, nullptr);
            break;
        case PdfLiteralDataType::Array:
            this->ReadArray(device, variant, nullptr);
            break;
        case PdfLiteralDataType::String:
            this->ReadString(device, variant, nullptr);
            break;
        case PdfLiteralDataType::HexString:
            this->ReadHexString(device, variant, nullptr);
            break;
        case PdfLiteralDataType::Name:
            this->ReadName(device, variant);
            break;
        case PdfLiteralDataType::Reference:
            PDFMM_RAISE_ERROR_INFO(PdfErrorCode::InternalLogic, "Unsupported reference datatype at this context");
        default:
            // Assume we have a keyword
            keyword = token;
            psTokenType = PdfPostScriptTokenType::Keyword;
            break;
    }

    return true;
}
