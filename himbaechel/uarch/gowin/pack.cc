#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"

#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "himbaechel_helpers.h"

#include "gowin.h"
#include "pack.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
struct GowinPacker
{
    Context *ctx;
    HimbaechelHelpers h;

    GowinPacker(Context *ctx) : ctx(ctx) { h.init(ctx); }

    // ===================================
    // IO
    // ===================================
    // create IOB connections for gowin_pack
    // can be called repeatedly when switching inputs, disabled outputs do not change
    void make_iob_nets(CellInfo &iob)
    {
        for (const auto &port : iob.ports) {
            const NetInfo *net = iob.getPort(port.first);
            std::string connected_net = "NET";
            if (net != nullptr) {
                if (ctx->verbose) {
                    log_info("%s: %s - %s\n", ctx->nameOf(&iob), port.first.c_str(ctx), ctx->nameOf(net));
                }
                if (net->name == ctx->id("$PACKER_VCC")) {
                    connected_net = "VCC";
                } else if (net->name == ctx->id("$PACKER_GND")) {
                    connected_net = "GND";
                }
                iob.setParam(ctx->idf("NET_%s", port.first.c_str(ctx)), connected_net);
            }
        }
    }

    void config_simple_io(CellInfo &ci)
    {
        if (ci.type.in(id_TBUF, id_IOBUF)) {
            return;
        }
        log_info("simple:%s\n", ctx->nameOf(&ci));
        ci.addInput(id_OEN);
        if (ci.type == id_OBUF) {
            ci.connectPort(id_OEN, ctx->nets[ctx->id("$PACKER_GND")].get());
        } else {
            NPNR_ASSERT(ci.type == id_IBUF);
            ci.connectPort(id_OEN, ctx->nets[ctx->id("$PACKER_VCC")].get());
        }
    }

    void config_bottom_row(CellInfo &ci, Loc loc, uint8_t cnd = Bottom_io_POD::NORMAL)
    {
        if (!have_bottom_io_cnds(ctx->chip_info)) {
            return;
        }
        if (!ci.type.in(id_OBUF, id_TBUF, id_IOBUF)) {
            return;
        }
        if (loc.z != BelZ::IOBA_Z) {
            return;
        }
        auto connect_io_wire = [&](IdString port, IdString net_name) {
            // XXX it is very convenient that nothing terrible happens in case
            // of absence/presence of a port
            ci.disconnectPort(port);
            ci.addInput(port);
            if (net_name == id_VSS) {
                ci.connectPort(port, ctx->nets[ctx->id("$PACKER_GND")].get());
            } else {
                NPNR_ASSERT(net_name == id_VCC);
                ci.connectPort(port, ctx->nets[ctx->id("$PACKER_VCC")].get());
            }
        };

        IdString wire_a_net = get_bottom_io_wire_a_net(ctx->chip_info, cnd);
        connect_io_wire(id_BOTTOM_IO_PORT_A, wire_a_net);

        IdString wire_b_net = get_bottom_io_wire_b_net(ctx->chip_info, cnd);
        connect_io_wire(id_BOTTOM_IO_PORT_B, wire_b_net);
    }

    // Attributes of deleted cells are copied
    void trim_nextpnr_iobs(void)
    {
        // Trim nextpnr IOBs - assume IO buffer insertion has been done in synthesis
        const pool<CellTypePort> top_ports{
                CellTypePort(id_IBUF, id_I),
                CellTypePort(id_OBUF, id_O),
                CellTypePort(id_TBUF, id_O),
                CellTypePort(id_IOBUF, id_IO),
        };
        std::vector<IdString> to_remove;
        for (auto &cell : ctx->cells) {
            auto &ci = *cell.second;
            if (!ci.type.in(ctx->id("$nextpnr_ibuf"), ctx->id("$nextpnr_obuf"), ctx->id("$nextpnr_iobuf")))
                continue;
            NetInfo *i = ci.getPort(ctx->id("I"));
            if (i && i->driver.cell) {
                if (!top_ports.count(CellTypePort(i->driver)))
                    log_error("Top-level port '%s' driven by illegal port %s.%s\n", ctx->nameOf(&ci),
                              ctx->nameOf(i->driver.cell), ctx->nameOf(i->driver.port));
                for (const auto &attr : ci.attrs) {
                    i->driver.cell->attrs[attr.first] = attr.second;
                }
            }
            NetInfo *o = ci.getPort(ctx->id("O"));
            if (o) {
                for (auto &usr : o->users) {
                    if (!top_ports.count(CellTypePort(usr)))
                        log_error("Top-level port '%s' driving illegal port %s.%s\n", ctx->nameOf(&ci),
                                  ctx->nameOf(usr.cell), ctx->nameOf(usr.port));
                    for (const auto &attr : ci.attrs) {
                        usr.cell->attrs[attr.first] = attr.second;
                    }
                }
            }
            NetInfo *io = ci.getPort(ctx->id("IO"));
            if (io && io->driver.cell) {
                if (!top_ports.count(CellTypePort(io->driver)))
                    log_error("Top-level port '%s' driven by illegal port %s.%s\n", ctx->nameOf(&ci),
                              ctx->nameOf(io->driver.cell), ctx->nameOf(io->driver.port));
                for (const auto &attr : ci.attrs) {
                    io->driver.cell->attrs[attr.first] = attr.second;
                }
            }
            ci.disconnectPort(ctx->id("I"));
            ci.disconnectPort(ctx->id("O"));
            ci.disconnectPort(ctx->id("IO"));
            to_remove.push_back(ci.name);
        }
        for (IdString cell_name : to_remove)
            ctx->cells.erase(cell_name);
    }

    BelId bind_io(CellInfo &ci)
    {
        BelId bel = ctx->getBelByName(IdStringList::parse(ctx, ci.attrs.at(id_BEL).as_string()));
        NPNR_ASSERT(bel != BelId());
        ci.unsetAttr(id_BEL);
        ctx->bindBel(bel, &ci, PlaceStrength::STRENGTH_LOCKED);
        return bel;
    }

    void pack_iobs(void)
    {
        log_info("Pack IOBs...\n");
        trim_nextpnr_iobs();

        for (auto &cell : ctx->cells) {
            CellInfo &ci = *cell.second;
            if (!ci.type.in(id_IBUF, id_OBUF, id_TBUF, id_IOBUF))
                continue;
            if (ci.attrs.count(id_BEL) == 0) {
                log_error("Unconstrained IO:%s\n", ctx->nameOf(&ci));
            }
            BelId io_bel = bind_io(ci);
            Loc io_loc = ctx->getBelLocation(io_bel);
            if (io_loc.y == ctx->getGridDimY() - 1) {
                config_bottom_row(ci, io_loc);
            }
            if (is_simple_io_bel(ctx->chip_info, io_bel)) {
                config_simple_io(ci);
            }
            make_iob_nets(ci);
        }
    }

    // ===================================
    // Differential IO
    // ===================================
    static bool is_iob(const Context *ctx, CellInfo *cell)
    {
        return (cell->type.in(id_IBUF, id_OBUF, id_TBUF, id_IOBUF));
    }

    std::pair<CellInfo *, CellInfo *> get_pn_cells(const CellInfo &ci)
    {
        CellInfo *p, *n;
        switch (ci.type.hash()) {
        case ID_ELVDS_TBUF: /* fall-through */
        case ID_TLVDS_TBUF: /* fall-through */
        case ID_ELVDS_OBUF: /* fall-through */
        case ID_TLVDS_OBUF:
            p = net_only_drives(ctx, ci.ports.at(id_O).net, is_iob, id_I, true);
            n = net_only_drives(ctx, ci.ports.at(id_OB).net, is_iob, id_I, true);
            break;
        case ID_ELVDS_IBUF: /* fall-through */
        case ID_TLVDS_IBUF:
            p = net_driven_by(ctx, ci.ports.at(id_I).net, is_iob, id_O);
            n = net_driven_by(ctx, ci.ports.at(id_IB).net, is_iob, id_O);
            break;
        case ID_ELVDS_IOBUF: /* fall-through */
        case ID_TLVDS_IOBUF:
            p = net_only_drives(ctx, ci.ports.at(id_IO).net, is_iob, id_I);
            n = net_only_drives(ctx, ci.ports.at(id_IOB).net, is_iob, id_I);
            break;
        default:
            log_error("Bad diff IO '%s' type '%s'\n", ctx->nameOf(&ci), ci.type.c_str(ctx));
        }
        return std::make_pair(p, n);
    }

    void mark_iobs_as_diff(CellInfo &ci, std::pair<CellInfo *, CellInfo *> &pn_cells)
    {
        pn_cells.first->setParam(id_DIFF, std::string("P"));
        pn_cells.first->setParam(id_DIFF_TYPE, ci.type.str(ctx));
        pn_cells.second->setParam(id_DIFF, std::string("N"));
        pn_cells.second->setParam(id_DIFF_TYPE, ci.type.str(ctx));
    }

    void switch_diff_ports(CellInfo &ci, std::pair<CellInfo *, CellInfo *> &pn_cells,
                           std::vector<IdString> &nets_to_remove)
    {
        CellInfo *iob_p = pn_cells.first;
        CellInfo *iob_n = pn_cells.second;

        if (ci.type.in(id_TLVDS_TBUF, id_TLVDS_OBUF, id_ELVDS_TBUF, id_ELVDS_OBUF)) {
            nets_to_remove.push_back(ci.getPort(id_O)->name);
            ci.disconnectPort(id_O);
            nets_to_remove.push_back(ci.getPort(id_OB)->name);
            ci.disconnectPort(id_OB);
            nets_to_remove.push_back(iob_n->getPort(id_I)->name);
            iob_n->disconnectPort(id_I);

            if (ci.type.in(id_TLVDS_TBUF, id_ELVDS_TBUF)) {
                nets_to_remove.push_back(iob_n->getPort(id_OEN)->name);
                iob_n->disconnectPort(id_OEN);
                iob_p->disconnectPort(id_OEN);
                ci.movePortTo(id_OEN, iob_p, id_OEN);
            }
            iob_p->disconnectPort(id_I);
            ci.movePortTo(id_I, iob_p, id_I);
            return;
        }
        if (ci.type.in(id_TLVDS_IBUF, id_ELVDS_IBUF)) {
            nets_to_remove.push_back(ci.getPort(id_I)->name);
            ci.disconnectPort(id_I);
            nets_to_remove.push_back(ci.getPort(id_IB)->name);
            ci.disconnectPort(id_IB);
            iob_n->disconnectPort(id_O);
            iob_p->disconnectPort(id_O);
            ci.movePortTo(id_O, iob_p, id_O);
            return;
        }
        if (ci.type.in(id_TLVDS_IOBUF, id_ELVDS_IOBUF)) {
            nets_to_remove.push_back(ci.getPort(id_IO)->name);
            ci.disconnectPort(id_IO);
            nets_to_remove.push_back(ci.getPort(id_IOB)->name);
            ci.disconnectPort(id_IOB);
            nets_to_remove.push_back(iob_n->getPort(id_I)->name);
            iob_n->disconnectPort(id_I);
            iob_n->disconnectPort(id_OEN);

            iob_p->disconnectPort(id_OEN);
            ci.movePortTo(id_OEN, iob_p, id_OEN);
            iob_p->disconnectPort(id_I);
            ci.movePortTo(id_I, iob_p, id_I);
            iob_p->disconnectPort(id_O);
            ci.movePortTo(id_O, iob_p, id_O);
            return;
        }
    }

    void pack_diff_iobs(void)
    {
        log_info("Pack diff IOBs...\n");
        std::vector<IdString> cells_to_remove, nets_to_remove;

        for (auto &cell : ctx->cells) {
            CellInfo &ci = *cell.second;
            if (!is_diffio(&ci)) {
                continue;
            }
            if (!is_diff_io_supported(ctx->chip_info, ci.type)) {
                log_error("%s is not supported\n", ci.type.c_str(ctx));
            }
            cells_to_remove.push_back(ci.name);
            auto pn_cells = get_pn_cells(ci);
            NPNR_ASSERT(pn_cells.first != nullptr && pn_cells.second != nullptr);

            mark_iobs_as_diff(ci, pn_cells);
            switch_diff_ports(ci, pn_cells, nets_to_remove);
        }

        for (auto cell : cells_to_remove) {
            ctx->cells.erase(cell);
        }
        for (auto net : nets_to_remove) {
            ctx->nets.erase(net);
        }
    }

    // ===================================
    // IO logic
    // ===================================
    // the functions of these two inputs are yet to be discovered, so we set as observed
    // in the exemplary images
    void set_daaj_nets(CellInfo &ci, BelId bel)
    {
        std::vector<IdString> pins = ctx->getBelPins(bel);
        if (std::find(pins.begin(), pins.end(), id_DAADJ0) != pins.end()) {
            ci.addInput(id_DAADJ0);
            ci.connectPort(id_DAADJ0, ctx->nets[ctx->id("$PACKER_GND")].get());
        }
        if (std::find(pins.begin(), pins.end(), id_DAADJ1) != pins.end()) {
            ci.addInput(id_DAADJ1);
            ci.connectPort(id_DAADJ1, ctx->nets[ctx->id("$PACKER_VCC")].get());
        }
    }

    BelId get_iologic_bel(CellInfo *iob)
    {
        NPNR_ASSERT(iob->bel != BelId());
        Loc loc = ctx->getBelLocation(iob->bel);
        loc.z = loc.z - BelZ::IOBA_Z + BelZ::IOLOGICA_Z;
        return ctx->getBelByLocation(loc);
    }

    void pack_bi_output_iol(CellInfo &ci, std::vector<IdString> &cells_to_remove, std::vector<IdString> &nets_to_remove)
    {
        // These primitives have an additional pin to control the tri-state iob - Q1.
        IdString out_port = id_Q0;
        IdString tx_port = id_Q1;

        CellInfo *out_iob = net_only_drives(ctx, ci.ports.at(out_port).net, is_iob, id_I);
        NPNR_ASSERT(out_iob != nullptr && out_iob->bel != BelId());
        BelId iob_bel = out_iob->bel;

        BelId l_bel = get_iologic_bel(out_iob);
        if (l_bel == BelId()) {
            log_error("Can't place IOLOGIC %s at %s\n", ctx->nameOf(&ci), ctx->nameOfBel(iob_bel));
        }

        if (!ctx->checkBelAvail(l_bel)) {
            log_error("Can't place %s at %s because it's already taken by %s\n", ctx->nameOf(&ci),
                      ctx->nameOfBel(l_bel), ctx->nameOf(ctx->getBoundBelCell(l_bel)));
        }
        ctx->bindBel(l_bel, &ci, PlaceStrength::STRENGTH_LOCKED);
        std::string out_mode;
        switch (ci.type.hash()) {
        case ID_ODDR:
        case ID_ODDRC:
            out_mode = "ODDRX1";
            break;
        case ID_OSER4:
            out_mode = "ODDRX2";
            break;
        case ID_OSER8:
            out_mode = "ODDRX4";
            break;
        }
        ci.setParam(ctx->id("OUTMODE"), out_mode);

        // mark IOB as used by IOLOGIC
        out_iob->setParam(id_IOLOGIC_IOB, 1);
        // disconnect Q output: it is wired internally
        nets_to_remove.push_back(ci.getPort(out_port)->name);
        out_iob->disconnectPort(id_I);
        ci.disconnectPort(out_port);
        set_daaj_nets(ci, iob_bel);
        config_bottom_row(*out_iob, ctx->getBelLocation(iob_bel), Bottom_io_POD::DDR);

        // if Q1 is connected then disconnect it too
        if (port_used(&ci, tx_port)) {
            NPNR_ASSERT(out_iob == net_only_drives(ctx, ci.ports.at(tx_port).net, is_iob, id_OEN));
            nets_to_remove.push_back(ci.getPort(tx_port)->name);
            out_iob->disconnectPort(id_OEN);
            ci.disconnectPort(tx_port);
        }
        make_iob_nets(*out_iob);
    }

    IdString create_aux_iologic_name(IdString main_name, int idx = 0)
    {
        std::string sfx("");
        if (idx) {
            sfx = std::to_string(idx);
        }
        return ctx->id(main_name.str(ctx) + std::string("_aux") + sfx);
    }

    BelId get_aux_iologic_bel(const CellInfo &ci)
    {
        return ctx->getBelByLocation(get_pair_iologic_bel(ctx->getBelLocation(ci.bel)));
    }

    void create_aux_iologic_cells(CellInfo &ci)
    {
        if (ci.type.in(id_ODDR, id_ODDRC, id_OSER4)) {
            return;
        }
        IdString aux_name = create_aux_iologic_name(ci.name);
        BelId bel = get_aux_iologic_bel(ci);
        ctx->createCell(aux_name, id_IOLOGIC_DUMMY);
        CellInfo *aux = ctx->cells[aux_name].get();
        aux->addInput(id_PCLK);
        ci.copyPortTo(id_PCLK, ctx->cells.at(aux_name).get(), id_PCLK);
        aux->addInput(id_FCLK);
        ci.copyPortTo(id_FCLK, ctx->cells.at(aux_name).get(), id_FCLK);
        aux->addInput(id_RESET);
        ci.copyPortTo(id_RESET, ctx->cells.at(aux_name).get(), id_RESET);
        ctx->cells.at(aux_name)->setParam(ctx->id("OUTMODE"), Property("DDRENABLE"));
        ctx->cells.at(aux_name)->setAttr(ctx->id("IOLOGIC_TYPE"), Property("DUMMY"));
        ctx->bindBel(bel, aux, PlaceStrength::STRENGTH_LOCKED);
    }

    void pack_iologic()
    {
        log_info("Pack IO logic...\n");
        std::vector<IdString> cells_to_remove, nets_to_remove;

        for (auto &cell : ctx->cells) {
            CellInfo &ci = *cell.second;
            if (!is_iologic(&ci)) {
                continue;
            }
            if (ctx->debug) {
                log_info("pack %s of type %s.\n", ctx->nameOf(&ci), ci.type.c_str(ctx));
            }
            if (ci.type.in(id_ODDR, id_ODDRC, id_OSER4, id_OSER8)) {
                pack_bi_output_iol(ci, cells_to_remove, nets_to_remove);
                create_aux_iologic_cells(ci);
                continue;
            }
        }

        for (auto cell : cells_to_remove) {
            ctx->cells.erase(cell);
        }
        for (auto net : nets_to_remove) {
            ctx->nets.erase(net);
        }
    }
    // ===================================
    // Constant nets
    // ===================================
    void handle_constants(void)
    {
        log_info("Create constant nets...\n");
        const dict<IdString, Property> vcc_params;
        const dict<IdString, Property> gnd_params;
        h.replace_constants(CellTypePort(id_GOWIN_VCC, id_V), CellTypePort(id_GOWIN_GND, id_G), vcc_params, gnd_params);

        // disconnect the constant LUT inputs
        log_info("Modify LUTs...\n");
        for (IdString netname : {ctx->id("$PACKER_GND"), ctx->id("$PACKER_VCC")}) {
            auto net = ctx->nets.find(netname);
            if (net == ctx->nets.end()) {
                continue;
            }
            NetInfo *constnet = net->second.get();
            for (auto user : constnet->users) {
                CellInfo *uc = user.cell;
                if (is_lut(uc) && (user.port.str(ctx).at(0) == 'I')) {
                    if (ctx->debug) {
                        log_info("%s user %s/%s\n", ctx->nameOf(constnet), ctx->nameOf(uc), user.port.c_str(ctx));
                    }

                    auto it_param = uc->params.find(id_INIT);
                    if (it_param == uc->params.end())
                        log_error("No initialization for lut found.\n");

                    int64_t uc_init = it_param->second.intval;
                    int64_t mask = 0;
                    uint8_t amt = 0;

                    if (user.port == id_I0) {
                        mask = 0x5555;
                        amt = 1;
                    } else if (user.port == id_I1) {
                        mask = 0x3333;
                        amt = 2;
                    } else if (user.port == id_I2) {
                        mask = 0x0F0F;
                        amt = 4;
                    } else if (user.port == id_I3) {
                        mask = 0x00FF;
                        amt = 8;
                    } else {
                        log_error("Port number invalid.\n");
                    }

                    if ((constnet->name == ctx->id("$PACKER_GND"))) {
                        uc_init = (uc_init & mask) | ((uc_init & mask) << amt);
                    } else {
                        uc_init = (uc_init & (mask << amt)) | ((uc_init & (mask << amt)) >> amt);
                    }

                    size_t uc_init_len = it_param->second.to_string().length();
                    uc_init &= (1LL << uc_init_len) - 1;

                    if (ctx->verbose && it_param->second.intval != uc_init)
                        log_info("%s lut config modified from 0x%lX to 0x%lX\n", ctx->nameOf(uc),
                                 it_param->second.intval, uc_init);

                    it_param->second = Property(uc_init, uc_init_len);
                    uc->disconnectPort(user.port);
                }
            }
        }
    }

    // ===================================
    // Wideluts
    // ===================================
    void pack_wideluts(void)
    {
        log_info("Pack wide LUTs...\n");
        // children's offsets
        struct _children
        {
            IdString port;
            int dx, dz;
        } mux_inputs[4][2] = {{{id_I0, 1, -7}, {id_I1, 0, -7}},
                              {{id_I0, 0, 4}, {id_I1, 0, -4}},
                              {{id_I0, 0, 2}, {id_I1, 0, -2}},
                              {{id_I0, 0, -BelZ::MUX20_Z}, {id_I1, 0, 2 - BelZ::MUX20_Z}}};
        typedef std::function<void(CellInfo &, CellInfo *, int, int)> recurse_func_t;
        recurse_func_t make_cluster = [&, this](CellInfo &ci_root, CellInfo *ci_cursor, int dx, int dz) {
            _children *inputs;
            if (is_lut(ci_cursor)) {
                return;
            }
            switch (ci_cursor->type.hash()) {
            case ID_MUX2_LUT8:
                inputs = mux_inputs[0];
                break;
            case ID_MUX2_LUT7:
                inputs = mux_inputs[1];
                break;
            case ID_MUX2_LUT6:
                inputs = mux_inputs[2];
                break;
            case ID_MUX2_LUT5:
                inputs = mux_inputs[3];
                break;
            default:
                log_error("Bad MUX2 node:%s\n", ctx->nameOf(ci_cursor));
            }
            for (int i = 0; i < 2; ++i) {
                // input src
                NetInfo *in = ci_cursor->getPort(inputs[i].port);
                NPNR_ASSERT(in && in->driver.cell && in->driver.cell->cluster == ClusterId());
                int child_dx = dx + inputs[i].dx;
                int child_dz = dz + inputs[i].dz;
                ci_root.constr_children.push_back(in->driver.cell);
                in->driver.cell->cluster = ci_root.name;
                in->driver.cell->constr_abs_z = false;
                in->driver.cell->constr_x = child_dx;
                in->driver.cell->constr_y = 0;
                in->driver.cell->constr_z = child_dz;
                make_cluster(ci_root, in->driver.cell, child_dx, child_dz);
            }
        };

        // look for MUX2
        // MUX2_LUT8 create right away, collect others
        std::vector<IdString> muxes[3];
        int packed[4] = {0, 0, 0, 0};
        for (auto &cell : ctx->cells) {
            auto &ci = *cell.second;
            if (ci.cluster != ClusterId()) {
                continue;
            }
            if (ci.type == id_MUX2_LUT8) {
                ci.cluster = ci.name;
                ci.constr_abs_z = false;
                make_cluster(ci, &ci, 0, 0);
                ++packed[0];
                continue;
            }
            if (ci.type.in(id_MUX2_LUT7, id_MUX2_LUT6, id_MUX2_LUT5)) {
                switch (ci.type.hash()) {
                case ID_MUX2_LUT7:
                    muxes[0].push_back(cell.first);
                    break;
                case ID_MUX2_LUT6:
                    muxes[1].push_back(cell.first);
                    break;
                default: // ID_MUX2_LUT5
                    muxes[2].push_back(cell.first);
                    break;
                }
            }
        }
        // create others
        for (int i = 0; i < 3; ++i) {
            for (IdString cell_name : muxes[i]) {
                auto &ci = *ctx->cells.at(cell_name);
                if (ci.cluster != ClusterId()) {
                    continue;
                }
                ci.cluster = ci.name;
                ci.constr_abs_z = false;
                make_cluster(ci, &ci, 0, 0);
                ++packed[i + 1];
            }
        }
        log_info("Packed MUX2_LUT8:%d, MUX2_LU7:%d, MUX2_LUT6:%d, MUX2_LUT5:%d\n", packed[0], packed[1], packed[2],
                 packed[3]);
    }

    // ===================================
    // ALU
    // ===================================
    // create ALU CIN block
    std::unique_ptr<CellInfo> alu_add_cin_block(Context *ctx, CellInfo *head, NetInfo *cin_net)
    {
        std::string name = head->name.str(ctx) + "_HEAD_ALULC";
        IdString name_id = ctx->id(name);

        NetInfo *cout_net = ctx->createNet(name_id);
        head->disconnectPort(id_CIN);
        head->connectPort(id_CIN, cout_net);

        auto cin_ci = std::make_unique<CellInfo>(ctx, name_id, id_ALU);
        cin_ci->addOutput(id_COUT);
        cin_ci->connectPort(id_COUT, cout_net);

        if (cin_net->name == ctx->id("$PACKER_GND")) {
            cin_ci->params[id_ALU_MODE] = std::string("C2L");
            cin_ci->addInput(id_I2);
            cin_ci->connectPort(id_I2, ctx->nets[ctx->id("$PACKER_VCC")].get());
            return cin_ci;
        }
        if (cin_net->name == ctx->id("$PACKER_VCC")) {
            cin_ci->params[id_ALU_MODE] = std::string("ONE2C");
            cin_ci->addInput(id_I2);
            cin_ci->connectPort(id_I2, ctx->nets[ctx->id("$PACKER_VCC")].get());
            return cin_ci;
        }
        // CIN from logic
        cin_ci->addInput(id_I1);
        cin_ci->addInput(id_I3);
        cin_ci->connectPort(id_I1, cin_net);
        cin_ci->connectPort(id_I3, cin_net);
        cin_ci->addInput(id_I2);
        cin_ci->connectPort(id_I2, ctx->nets[ctx->id("$PACKER_VCC")].get());
        cin_ci->params[id_ALU_MODE] = std::string("0"); // ADD
        return cin_ci;
    }

    // create ALU COUT block
    std::unique_ptr<CellInfo> alu_add_cout_block(Context *ctx, CellInfo *tail, NetInfo *cout_net)
    {
        std::string name = tail->name.str(ctx) + "_TAIL_ALULC";
        IdString name_id = ctx->id(name);

        NetInfo *cin_net = ctx->createNet(name_id);
        tail->disconnectPort(id_COUT);
        tail->connectPort(id_COUT, cin_net);

        auto cout_ci = std::make_unique<CellInfo>(ctx, name_id, id_ALU);
        cout_ci->addOutput(id_COUT); // may be needed for the ALU filler
        cout_ci->addInput(id_CIN);
        cout_ci->connectPort(id_CIN, cin_net);
        cout_ci->addOutput(id_SUM);
        cout_ci->connectPort(id_SUM, cout_net);
        cout_ci->addInput(id_I2);
        cout_ci->connectPort(id_I2, ctx->nets[ctx->id("$PACKER_VCC")].get());

        cout_ci->params[id_ALU_MODE] = std::string("C2L");
        return cout_ci;
    }

    // create ALU filler block
    std::unique_ptr<CellInfo> alu_add_dummy_block(Context *ctx, CellInfo *tail)
    {
        std::string name = tail->name.str(ctx) + "_DUMMY_ALULC";
        IdString name_id = ctx->id(name);

        auto dummy_ci = std::make_unique<CellInfo>(ctx, name_id, id_ALU);
        dummy_ci->params[id_ALU_MODE] = std::string("C2L");
        return dummy_ci;
    }

    // create ALU chain
    void pack_alus(void)
    {
        const CellTypePort cell_alu_cout = CellTypePort(id_ALU, id_COUT);
        const CellTypePort cell_alu_cin = CellTypePort(id_ALU, id_CIN);
        std::vector<std::unique_ptr<CellInfo>> new_cells;

        log_info("Pack ALUs...\n");
        for (auto &cell : ctx->cells) {
            auto ci = cell.second.get();
            if (ci->cluster != ClusterId()) {
                continue;
            }
            if (is_alu(ci)) {
                // The ALU head is when the input carry is not a dedicated wire from the previous ALU
                NetInfo *cin_net = ci->getPort(id_CIN);
                if (!cin_net || !cin_net->driver.cell) {
                    log_error("CIN disconnected at ALU:%s\n", ctx->nameOf(ci));
                }
                if (CellTypePort(cin_net->driver) != cell_alu_cout || cin_net->users.entries() > 1) {
                    if (ctx->debug) {
                        log_info("ALU head found %s. CIN net is %s\n", ctx->nameOf(ci), ctx->nameOf(cin_net));
                    }
                    // always prepend first ALU with carry generator block
                    // three cases: CIN == 0, CIN == 1 and CIN == ?
                    new_cells.push_back(std::move(alu_add_cin_block(ctx, ci, cin_net)));
                    CellInfo *cin_block_ci = new_cells.back().get();
                    // CIN block is the cluster root and is always placed in ALU0
                    // This is a possible place for further optimization
                    cin_block_ci->cluster = cin_block_ci->name;
                    cin_block_ci->constr_z = BelZ::ALU0_Z;
                    cin_block_ci->constr_abs_z = true;

                    int alu_chain_len = 1;
                    while (true) {
                        // add to cluster
                        if (ctx->debug) {
                            log_info("Add ALU to the chain (len:%d): %s\n", alu_chain_len, ctx->nameOf(ci));
                        }
                        cin_block_ci->constr_children.push_back(ci);
                        NPNR_ASSERT(ci->cluster == ClusterId());
                        ci->cluster = cin_block_ci->name;
                        ci->constr_abs_z = false;
                        ci->constr_x = alu_chain_len / 6;
                        ci->constr_y = 0;
                        ci->constr_z = alu_chain_len % 6;
                        // XXX I2 is pin C which must be set to 1 for all ALU modes except MUL
                        // we use only mode 2 ADDSUB so create and connect this pin
                        ci->addInput(id_I2);
                        ci->connectPort(id_I2, ctx->nets[ctx->id("$PACKER_VCC")].get());

                        ++alu_chain_len;

                        // check for the chain end
                        NetInfo *cout_net = ci->getPort(id_COUT);
                        if (!cout_net || cout_net->users.empty()) {
                            break;
                        }
                        if (CellTypePort(*cout_net->users.begin()) != cell_alu_cin || cout_net->users.entries() > 1) {
                            new_cells.push_back(std::move(alu_add_cout_block(ctx, ci, cout_net)));
                            CellInfo *cout_block_ci = new_cells.back().get();
                            cin_block_ci->constr_children.push_back(cout_block_ci);
                            NPNR_ASSERT(cout_block_ci->cluster == ClusterId());
                            cout_block_ci->cluster = cin_block_ci->name;
                            cout_block_ci->constr_abs_z = false;
                            cout_block_ci->constr_x = alu_chain_len / 6;
                            cout_block_ci->constr_y = 0;
                            cout_block_ci->constr_z = alu_chain_len % 6;
                            if (ctx->debug) {
                                log_info("Add ALU carry out to the chain (len:%d): %s COUT-net: %s\n", alu_chain_len,
                                         ctx->nameOf(cout_block_ci), ctx->nameOf(cout_net));
                            }

                            ++alu_chain_len;

                            break;
                        }
                        ci = (*cout_net->users.begin()).cell;
                    }
                    // ALUs are always paired
                    if (alu_chain_len & 1) {
                        // create dummy cell
                        new_cells.push_back(std::move(alu_add_dummy_block(ctx, ci)));
                        CellInfo *dummy_block_ci = new_cells.back().get();
                        cin_block_ci->constr_children.push_back(dummy_block_ci);
                        NPNR_ASSERT(dummy_block_ci->cluster == ClusterId());
                        dummy_block_ci->cluster = cin_block_ci->name;
                        dummy_block_ci->constr_abs_z = false;
                        dummy_block_ci->constr_x = alu_chain_len / 6;
                        dummy_block_ci->constr_y = 0;
                        dummy_block_ci->constr_z = alu_chain_len % 6;
                        if (ctx->debug) {
                            log_info("Add ALU dummy cell to the chain (len:%d): %s\n", alu_chain_len,
                                     ctx->nameOf(dummy_block_ci));
                        }
                    }
                }
            }
        }
        for (auto &ncell : new_cells) {
            ctx->cells[ncell->name] = std::move(ncell);
        }
    }

    // ===================================
    // glue LUT and FF
    // ===================================
    void constrain_lutffs(void)
    {
        // Constrain directly connected LUTs and FFs together to use dedicated resources
        const pool<CellTypePort> lut_outs{{id_LUT1, id_F}, {id_LUT2, id_F}, {id_LUT3, id_F}, {id_LUT4, id_F}};
        const pool<CellTypePort> dff_ins{{id_DFF, id_D},  {id_DFFE, id_D},  {id_DFFN, id_D},  {id_DFFNE, id_D},
                                         {id_DFFS, id_D}, {id_DFFSE, id_D}, {id_DFFNS, id_D}, {id_DFFNSE, id_D},
                                         {id_DFFR, id_D}, {id_DFFRE, id_D}, {id_DFFNR, id_D}, {id_DFFNRE, id_D},
                                         {id_DFFP, id_D}, {id_DFFPE, id_D}, {id_DFFNP, id_D}, {id_DFFNPE, id_D},
                                         {id_DFFC, id_D}, {id_DFFCE, id_D}, {id_DFFNC, id_D}, {id_DFFNCE, id_D}};

        int lutffs = h.constrain_cell_pairs(lut_outs, dff_ins, 1);
        log_info("Constrained %d LUTFF pairs.\n", lutffs);
    }

    // ===================================
    // SSRAM cluster
    // ===================================
    // create ALU filler block
    std::unique_ptr<CellInfo> ssram_make_lut(Context *ctx, CellInfo *ci, int index)
    {
        IdString name_id = ctx->idf("%s_LUT%d", ci->name.c_str(ctx), index);
        auto lut_ci = std::make_unique<CellInfo>(ctx, name_id, id_LUT4);
        if (index) {
            for (IdString port : {id_I0, id_I1, id_I2, id_I3}) {
                lut_ci->addInput(port);
            }
        }
        IdString init_name = ctx->idf("INIT_%d", index);
        if (ci->params.count(init_name)) {
            lut_ci->params[id_INIT] = ci->params.at(init_name);
        } else {
            lut_ci->params[id_INIT] = std::string("1111111111111111");
        }
        return lut_ci;
    }

    void pack_ram16sdp4(void)
    {
        std::vector<std::unique_ptr<CellInfo>> new_cells;

        log_info("Pack RAMs...\n");
        for (auto &cell : ctx->cells) {
            auto ci = cell.second.get();
            if (ci->cluster != ClusterId()) {
                continue;
            }

            if (is_ssram(ci)) {
                // make cluster root
                ci->cluster = ci->name;
                ci->constr_abs_z = true;
                ci->constr_x = 0;
                ci->constr_y = 0;
                ci->constr_z = BelZ::RAMW_Z;

                ci->addInput(id_CE);
                ci->connectPort(id_CE, ctx->nets[ctx->id("$PACKER_VCC")].get());

                // RAD networks
                NetInfo *rad[4];
                for (int i = 0; i < 4; ++i) {
                    rad[i] = ci->getPort(ctx->idf("RAD[%d]", i));
                }

                // active LUTs
                int luts_num = 4;
                if (ci->type == id_RAM16SDP1) {
                    luts_num = 1;
                } else {
                    if (ci->type == id_RAM16SDP2) {
                        luts_num = 2;
                    }
                }

                // make actual storage cells
                for (int i = 0; i < 4; ++i) {
                    new_cells.push_back(std::move(ssram_make_lut(ctx, ci, i)));
                    CellInfo *lut_ci = new_cells.back().get();
                    ci->constr_children.push_back(lut_ci);
                    lut_ci->cluster = ci->name;
                    lut_ci->constr_abs_z = true;
                    lut_ci->constr_x = 0;
                    lut_ci->constr_y = 0;
                    lut_ci->constr_z = i * 2;
                    // inputs
                    // LUT0 is already connected when generating the base
                    if (i && i < luts_num) {
                        for (int j = 0; j < 4; ++j) {
                            lut_ci->connectPort(ctx->idf("I%d", j), rad[j]);
                        }
                    }
                }
            }
        }
        for (auto &ncell : new_cells) {
            ctx->cells[ncell->name] = std::move(ncell);
        }
    }

    // ===================================
    // Global set/reset
    // ===================================
    void pack_gsr(void)
    {
        log_info("Pack GSR..\n");

        bool user_gsr = false;
        for (auto &cell : ctx->cells) {
            auto &ci = *cell.second;

            if (ci.type == id_GSR) {
                user_gsr = true;
                break;
            }
        }
        if (!user_gsr) {
            bool have_gsr_bel = false;
            auto bels = ctx->getBels();
            for (auto bid : bels) {
                if (ctx->getBelType(bid) == id_GSR) {
                    have_gsr_bel = true;
                    break;
                }
            }
            if (have_gsr_bel) {
                // make default GSR
                auto gsr_cell = std::make_unique<CellInfo>(ctx, id_GSR, id_GSR);
                gsr_cell->addInput(id_GSRI);
                gsr_cell->connectPort(id_GSRI, ctx->nets[ctx->id("$PACKER_VCC")].get());
                ctx->cells[gsr_cell->name] = std::move(gsr_cell);
            } else {
                log_info("No GSR in the chip base\n");
            }
        }
    }

    // ===================================
    // PLL
    // ===================================
    void pack_pll(void)
    {
        log_info("Pack PLL..\n");

        for (auto &cell : ctx->cells) {
            auto &ci = *cell.second;

            if (ci.type == id_rPLL) {
                // pin renaming for compatibility
                for (int i = 0; i < 6; ++i) {
                    ci.renamePort(ctx->idf("FBDSEL[%d]", i), ctx->idf("FBDSEL%d", i));
                    ci.renamePort(ctx->idf("IDSEL[%d]", i), ctx->idf("IDSEL%d", i));
                    ci.renamePort(ctx->idf("ODSEL[%d]", i), ctx->idf("ODSEL%d", i));
                    if (i < 4) {
                        ci.renamePort(ctx->idf("PSDA[%d]", i), ctx->idf("PSDA%d", i));
                        ci.renamePort(ctx->idf("DUTYDA[%d]", i), ctx->idf("DUTYDA%d", i));
                        ci.renamePort(ctx->idf("FDLY[%d]", i), ctx->idf("FDLY%d", i));
                    }
                }
            }
        }
    }

    void run(void)
    {
        handle_constants();
        pack_iobs();
        pack_diff_iobs();
        pack_iologic();
        pack_gsr();
        pack_wideluts();
        pack_alus();
        constrain_lutffs();
        pack_pll();
        pack_ram16sdp4();

        ctx->fixupHierarchy();
        ctx->check();
    }
};
} // namespace

void gowin_pack(Context *ctx)
{
    GowinPacker packer(ctx);
    packer.run();
}

NEXTPNR_NAMESPACE_END