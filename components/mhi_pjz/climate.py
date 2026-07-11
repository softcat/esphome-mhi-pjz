import esphome.codegen as cg
from esphome.components import climate_ir

AUTO_LOAD = ["climate_ir"]
CODEOWNERS = ["@softcat"]

mhi_pjz_ns = cg.esphome_ns.namespace("mhi_pjz")
MhiPjzClimate = mhi_pjz_ns.class_("MhiPjzClimate", climate_ir.ClimateIR)

CONFIG_SCHEMA = climate_ir.climate_ir_with_receiver_schema(MhiPjzClimate)


async def to_code(config):
    await climate_ir.new_climate_ir(config)
