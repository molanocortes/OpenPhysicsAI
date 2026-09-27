# Batch 3 sources: the documents the common engineering metal records were read from

Every value in the batch-3 records of [src/ctl/materials.json](../src/ctl/materials.json) names one of these
documents in its `source`. Each was downloaded on 2026-09-20 and read; the checksum is of the file as downloaded.
A supplier data sheet is cited for its facts, no table copied ([README.md](README.md)). These are wrought and cast
metals: no record in this batch carries an as-built additive value.

| document | URL | terms | SHA-256 of the download |
|---|---|---|---|
| Ovako, Steel Navigator, grade S235JR to EN 10025-2:2004, Mechanical Properties and Other properties tables | <https://steelnavigator.ovako.com/steel-grades/s235/> | steel producer's grade data: values cited as facts with the document, no table copied | `2ee2b1514fdcbfd6` |
| Ovako, Steel Navigator, grade S355JR to EN 10025-2:2019, Mechanical Properties and Other properties tables | <https://steelnavigator.ovako.com/steel-grades/s355/> | steel producer's grade data: values cited as facts with the document, no table copied | `154beffccf607992` |
| Aalco Metals, technical datasheet Aluminium Alloy 6061-T6 Extrusions, Properties table and the BS EN 755-2:2008 extrusions table | <https://www.aalco.co.uk/datasheets/Aluminium-Alloy-6061-T6-Extrusions_145.ashx> | supplier data sheet: values cited as facts with the document, no table copied | `88b64b1378ceae51` |
| Aalco Metals, technical datasheet Aluminium Alloy 1050A H14 Sheet, Properties table and the BS EN 485-2:2008 sheet table | <https://www.aalco.co.uk/datasheets/Aluminium-Alloy-1050A-H14-Sheet_57.ashx> | supplier data sheet: values cited as facts with the document, no table copied | `04565d4fa68238be` |
| Aalco Metals, technical datasheet Stainless Steel 1.4301 (304) Sheet and Plate, Properties table and the EN 10088-2:2005 sheet and plate tables | <https://www.aalco.co.uk/datasheets/Stainless-Steel-14301-Sheet-and-Plate-Quarto-Plate--CPP-Plate_343.ashx> | supplier data sheet: values cited as facts with the document, no table copied | `0911d0541abf62bd` |
| Aalco Metals, technical datasheet Copper and Copper Alloys, Brass CW614N Brass Rod, Properties table and the EN 12164:2011 bar table | <https://www.aalco.co.uk/datasheets/Copper-and-Copper-Alloys-CW614N-Brass-Rod_31.ashx> | supplier data sheet: values cited as facts with the document, no table copied | `fd7f36f0fb1ad8cf` |
| Aalco Metals, technical datasheet Copper and Copper Alloys, Aluminium Bronze CW307G, Properties table | <https://www.aalco.co.uk/datasheets/Copper-and-Copper-Alloys-CW307G_445.ashx> | supplier data sheet: values cited as facts with the document, no table copied | `64bdef38e4346ec0` |
| NIST Material Measurement Laboratory, cryogenic material properties database, 6061-T6 aluminium: the log10 fits for thermal conductivity and specific heat and the polynomial fit for Young's modulus, with their coefficients, ranges and stated errors | <https://trc.nist.gov/cryogenics/materials/6061%20Aluminum/6061_T6Aluminum_rev.htm> | US government work, public domain | `a965b6ca7ed9bac2` |
| NIST Material Measurement Laboratory, cryogenic material properties database, 304 stainless steel: the log10 fits for thermal conductivity and specific heat with their coefficients, ranges and stated errors | <https://trc.nist.gov/cryogenics/materials/304Stainless/304Stainless_rev.htm> | US government work, public domain | `b713259d3aa51b4b` |
| Measurement of Third-Order Elastic Constants Using Thermal Modulation of Ultrasonic Waves, arXiv:2608.12244, experimental section (the aluminium 6061 block, its density, Poisson's ratio and measured wave speeds) | <https://arxiv.org/html/2608.12244> | arXiv.org perpetual non-exclusive license | `87e8faf5b0628331` |
| High-Power Fiber Laser Welding of High-Strength AA7075-T6 Aluminum Alloy Welds for Mechanical Properties Research, PMC8705241, Table 1 (base metal of 6 mm commercial sheet) | <https://pmc.ncbi.nlm.nih.gov/articles/PMC8705241/> | CC BY 4.0 | `28e8f35ba7857512` |
| Behavior of Defective Aluminum Panels Under Shear Forces Patched with Composite Plates, PMC12430867, Section 2.2 Materials and Properties (the elastic values it states for 7075-T6) | <https://pmc.ncbi.nlm.nih.gov/articles/PMC12430867/> | CC BY 4.0 | `cb24cfab2649f288` |
| Tensile Deformation and Fracture of Unreinforced AZ91 and Reinforced AZ91-C at Temperatures up to 300 degC, PMC10342948, Table 1 (general properties), Table 2 (composition) and the measured tensile values at 20 degC | <https://www.ncbi.nlm.nih.gov/pmc/articles/PMC10342948/> | CC BY 4.0 | `8b36b002f0daeefe` |
| Cast Iron Parts Obtained in Ceramic Molds Produced by Binder Jetting 3D Printing, Morphological and Mechanical Characterization, PMC8402146, Table 1 (20 tensile tests on EN-GJL-250 to SR EN 10002-1:1990), Table 3 (the reference values compared against) and Table 4 (composition) | <https://pmc.ncbi.nlm.nih.gov/articles/PMC8402146/> | CC BY 4.0 | `c213ef84284b8912` |
| Wikipedia, 7075 aluminium alloy, revision 1375345274 of 2026-09-17, infobox Physical properties (density only) | <https://en.wikipedia.org/w/index.php?title=7075_aluminium_alloy&oldid=1375345274> | CC BY-SA, cited by article and revision | `200ba08a88d9ca8f` |
| Boro Foundry, Grey Iron Casting Grades, Specifications to EN 1561 (the EN-GJL-250 row) | <https://borofoundry.co.uk/grey-iron-grades> | foundry's published grade table: values cited as facts with the document, no table copied | `eea0dd26c5d6238a` |
| Grey Cast Iron EN-JL1040 and EN-GJL-250, citing EN 1561:1997 Founding. Grey cast irons (density and the grade's strength range) | <https://www.iron-foundry.com/Grey-Cast-Iron-EN-JL1040-GJL-250.html> | foundry's published grade page: values cited as facts with the document, no table copied | `295b395f07c605d2` |

Two things about the checksums. These are live HTML pages, not fixed PDFs, so a page may differ from one download
to the next and the checksum identifies the copy that was read on the access date, not a stable artefact. And
`pmc.ncbi.nlm.nih.gov` answered the plain client used for checksumming with a short holding page for the AZ91
article however long it was left between tries, so that one document is cited and checksummed at
`www.ncbi.nlm.nih.gov`, which served the article itself; the other three PMC articles answered normally.

## What these documents do not give

A missing value is left missing rather than borrowed from a neighbouring alloy, so these gaps are in the records as
absent keys and are listed in each record's `not_modelled`:

- Poisson's ratio for 1050A, 1.4301, CW614N, CW307G, EN-GJL-250 and AZ91. Only the two structural steels (Ovako)
  and 7075-T6 (PMC12430867) have one from a document, and 6061 has one from the ultrasonic measurement in
  arXiv:2608.12244, whose temper is not stated.
- Specific heat for 1050A, CW614N, CW307G, EN-GJL-250, AZ91 and 7075-T6. For 6061-T6 and 1.4301 it is evaluated
  from the NIST fits at 293.15 K and labelled `inferred`, not `published`.
- Every thermal property of 7075-T6, and thermal conductivity, specific heat and expansion for EN-GJL-250.
- Thermal expansion for CW307G.
- Any temperature dependence at all: every value in this batch is a room temperature value, and the two expansion
  coefficients that name an interval (12e-6 over 20 to 300 degC for the steels, 21e-6 over 50 to 200 degC for AZ91)
  are mean coefficients over that interval, not tangent values.
- Emissivity and latent heat for all ten, and the solidus and liquidus for all ten: where a melting temperature is
  given at all it is a single number, recorded as `melting_c`.

One value needs reading before it is used. The modulus of grey cast iron EN-GJL-250 is recorded as the 37.0 GPa
measured in PMC8402146, because that is the traceable measurement; the same paper's Table 3 carries 120 GPa for the
grade from a commercial software library whose own source cannot be checked. The record says so in its note. A part
analysed with 37 GPa will deflect about three times as far as one analysed with 120 GPa, so this is the first value
to replace when a better source or a measurement of our own arrives.
