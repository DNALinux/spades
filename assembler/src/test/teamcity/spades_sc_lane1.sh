#!/bin/bash

############################################################################
# Copyright (c) 2011-2012 Saint-Petersburg Academic University
# All Rights Reserved
# See file LICENSE for details.
############################################################################

set -e
pushd ../../../

rm -rf spades_output/ECOLI_SC_LANE_1_BH_woHUMAN

#./spades_compile.sh
./spades.py --config-file src/test/teamcity/spades_config_sc_lane1.info

rm -rf ~/quast-1.1/ECOLI_SC_LANE_1_BH_woHUMAN/

python ~/quast-1.1/quast.py -R data/input/E.coli/MG1655-K12.fasta.gz -G data/input/E.coli/genes/genes.gff -O data/input/E.coli/genes/operons.gff -o ~/quast-1.1/ECOLI_SC_LANE_1_BH_woHUMAN/ spades_output/ECOLI_SC_LANE_1_BH_woHUMAN/contigs.fasta

python src/test/teamcity/assess.py ~/quast-1.1/ECOLI_SC_LANE_1_BH_woHUMAN/transposed_report.tsv 85000 7
exitlvl=$?
popd

if [ $exitlvl -ne 0 ]; then
    exit $exitlvl
else
    etalon=/smallnas/teamcity/etalon_output/ECOLI_SC_LANE_1_BH_woHUMAN/etalon
    ./detect_diffs.sh ../../../spades_output/ECOLI_SC_LANE_1_BH_woHUMAN $etalon
    exit $?
fi
