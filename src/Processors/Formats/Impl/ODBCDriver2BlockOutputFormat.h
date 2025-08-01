#pragma once

#include <Processors/Formats/IOutputFormat.h>
#include <Formats/FormatSettings.h>

#include <string>

namespace DB
{
class Block;
class WriteBuffer;


/** A data format designed to simplify the implementation of the ODBC driver.
  * ODBC driver is designed to be build for different platforms without dependencies from the main code,
  *  so the format is made that way so that it can be as easy as possible to parse it.
  * A header is displayed with the required information.
  * The data is then output in the order of the rows. Each value is displayed as follows: length in Int32 format (-1 for NULL), then data in text form.
  */
class ODBCDriver2BlockOutputFormat final : public IOutputFormat
{
public:
    ODBCDriver2BlockOutputFormat(WriteBuffer & out_, SharedHeader header_, const FormatSettings & format_settings_);

    String getName() const override { return "ODBCDriver2"; }

private:
    void consume(Chunk) override;
    void consumeTotals(Chunk) override;
    void writePrefix() override;

    const FormatSettings format_settings;
    Serializations serializations;

    void writeRow(const Columns & columns, size_t row_idx, std::string & buffer);
    void write(Chunk chunk, PortKind port_kind);
};


}
