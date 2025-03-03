//***************************************************************************
//* Copyright (c) 2023-2024 SPAdes team
//* Copyright (c) 2015-2022 Saint Petersburg State University
//* Copyright (c) 2011-2014 Saint Petersburg Academic University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "utils/stl_utils.hpp"
#include "utils/logger/log_writers.hpp"

#include "io/reads/file_reader.hpp"
#include "io/reads/wrapper_collection.hpp"
#include "io/reads/osequencestream.hpp"

using namespace std;

void create_console_logger() {
    logging::logger *log = logging::create_logger("", logging::L_INFO);
    log->add_writer(std::make_shared<logging::console_writer>());
    logging::attach_logger(log);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        cout << "Usage: reference_fixer <reference path> <output path>" << endl;
        return 1;
    }
    create_console_logger();
    filesystem::path fn = argv[1];
    CHECK_FATAL_ERROR(exists(fn), "File " << fn << " doesn't exist or can't be read!");
    string out_fn = argv[2];
    auto reader = io::NonNuclCollapsingWrapper(io::FileReadStream(fn));
    io::SingleRead read;
    std::stringstream ss;
    while (!reader.eof()) {
        reader >> read;
        ss << read.GetSequenceString();
    }
    io::SingleRead concat("concat", ss.str());
    io::OFastaReadStream out(out_fn);
    out << concat;
    return 0;
}
