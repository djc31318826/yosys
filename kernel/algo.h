/* -*- c++ -*-
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#ifndef SATGEN_ALGO_H
#define SATGEN_ALGO_H

#include "kernel/celltypes.h"
#include "kernel/rtlil.h"
#include "kernel/sigtools.h"
#include <stack>

YOSYS_NAMESPACE_BEGIN

CellTypes comb_cells_filt()
{
	CellTypes ct;

	ct.setup_internals();
	ct.setup_stdcells();

	return ct;
}

struct Netlist {
	RTLIL::Module *module;
	SigMap sigmap;
	dict<RTLIL::SigBit, Cell *> sigbit_driver_map;
	dict<RTLIL::Cell *, std::set<RTLIL::SigBit>> cell_inputs_map;

	Netlist(RTLIL::Module *module) : module(module), sigmap(module)
	{
		CellTypes ct(module->design);
		setup_netlist(module, ct);
	}

	Netlist(RTLIL::Module *module, const CellTypes &ct) : module(module), sigmap(module) { setup_netlist(module, ct); }

	void setup_netlist(RTLIL::Module *module, const CellTypes &ct)
	{
		for (auto cell : module->cells()) {
			if (ct.cell_known(cell->type)) {
				std::set<RTLIL::SigBit> inputs, outputs;
				for (auto &port : cell->connections()) {
					std::vector<RTLIL::SigBit> bits = sigmap(port.second).to_sigbit_vector();
					if (ct.cell_output(cell->type, port.first))
						outputs.insert(bits.begin(), bits.end());
					else
						inputs.insert(bits.begin(), bits.end());
				}
				cell_inputs_map[cell] = inputs;
				for (auto &bit : outputs) {
					sigbit_driver_map[bit] = cell;
				};
			}
		}
	}
};

namespace detail
{
struct NetlistConeWireIter : public std::iterator<std::input_iterator_tag, RTLIL::SigBit> {
	using set_iter_t = std::set<RTLIL::SigBit>::iterator;

	const Netlist &net;
	RTLIL::SigBit sig;
	bool sentinel;

	std::stack<std::pair<set_iter_t, set_iter_t>> dfs_path_stack;
	std::set<RTLIL::Cell *> cells_visited;

	NetlistConeWireIter(const Netlist &net) : net(net), sentinel(true) {}

	NetlistConeWireIter(const Netlist &net, RTLIL::SigBit sig) : net(net), sig(sig), sentinel(false) {}

	const RTLIL::SigBit &operator*() const { return sig; };
	bool operator!=(const NetlistConeWireIter &other) const
	{
		if (sentinel || other.sentinel) {
			return sentinel != other.sentinel;
		} else {
			return sig != other.sig;
		}
	}

	bool operator==(const NetlistConeWireIter &other) const {
		if (sentinel || other.sentinel) {
			return sentinel == other.sentinel;
		} else {
			return sig == other.sig;
		}
	}

	void next_sig_in_dag()
	{
		while (1) {
			if (dfs_path_stack.empty()) {
				sentinel = true;
				return;
			}

			auto &cell_inputs_iter = dfs_path_stack.top().first;
			auto &cell_inputs_iter_guard = dfs_path_stack.top().second;

			cell_inputs_iter++;
			if (cell_inputs_iter != cell_inputs_iter_guard) {
				sig = *cell_inputs_iter;
				return;
			} else {
				dfs_path_stack.pop();
			}
		}
	}

	NetlistConeWireIter &operator++()
	{
		if (net.sigbit_driver_map.count(sig)) {
			auto drv = net.sigbit_driver_map.at(sig);

			if (!cells_visited.count(drv)) {
				auto &inputs = net.cell_inputs_map.at(drv);
				dfs_path_stack.push(std::make_pair(inputs.begin(), inputs.end()));
				cells_visited.insert(drv);
				sig = (*dfs_path_stack.top().first);
			} else {
				next_sig_in_dag();
			}
		} else {
			next_sig_in_dag();
		}
		return *this;
	}
};

struct NetlistConeWireIterable {
	const Netlist &net;
	RTLIL::SigBit sig;

	NetlistConeWireIterable(const Netlist &net, RTLIL::SigBit sig) : net(net), sig(sig) {}

	NetlistConeWireIter begin() { return NetlistConeWireIter(net, sig); }
	NetlistConeWireIter end() { return NetlistConeWireIter(net); }
};

struct NetlistConeCellIter : public std::iterator<std::input_iterator_tag, RTLIL::Cell *> {
	const Netlist &net;

	NetlistConeWireIter sig_iter;

	NetlistConeCellIter(const Netlist &net) : net(net), sig_iter(net) {}

	NetlistConeCellIter(const Netlist &net, RTLIL::SigBit sig) : net(net), sig_iter(net, sig)
	{
		if ((!sig_iter.sentinel) && (!has_driver_cell(*sig_iter))) {
			++(*this);
		}
	}

	bool has_driver_cell(const RTLIL::SigBit &s) { return net.sigbit_driver_map.count(s); }

	RTLIL::Cell *operator*() const { return net.sigbit_driver_map.at(*sig_iter); };

	bool operator!=(const NetlistConeCellIter &other) const { return sig_iter != other.sig_iter; }
	bool operator==(const NetlistConeCellIter &other) const { return sig_iter == other.sig_iter; }
	NetlistConeCellIter &operator++()
	{
		while (true) {
			++sig_iter;
			if (sig_iter.sentinel) {
				return *this;
			}

			if (has_driver_cell(*sig_iter)) {
				auto cell = net.sigbit_driver_map.at(*sig_iter);

				if (!sig_iter.cells_visited.count(cell)) {
					return *this;
				}
			}
		};
	}
};

struct NetlistConeCellIterable {
	const Netlist &net;
	RTLIL::SigBit sig;

	NetlistConeCellIterable(const Netlist &net, RTLIL::SigBit sig) : net(net), sig(sig) {}

	NetlistConeCellIter begin() { return NetlistConeCellIter(net, sig); }
	NetlistConeCellIter end() { return NetlistConeCellIter(net); }
};

// struct NetlistConeInputsIter : public std::iterator<std::input_iterator_tag, const RTLIL::Cell *> {
// 	const Netlist &net;
// 	RTLIL::SigBit sig;

// 	NetlistConeWireIter sig_iter;

// 	bool has_driver_cell(const RTLIL::SigBit &s) { return net.sigbit_driver_map.count(s); }

// 	NetlistConeInputsIter(const Netlist &net, RTLIL::SigBit sig = NULL) : net(net), sig(sig), sig_iter(net, sig)
// 	{
// 		if ((sig != NULL) && (has_driver_cell(sig_iter))) {
// 			++(*this);
// 		}
// 	}

// 	const RTLIL::SigBit &operator*() const { return sig_iter; };
// 	bool operator!=(const NetlistConeInputsIter &other) const { return sig_iter != other.sig_iter; }
// 	bool operator==(const NetlistConeInputsIter &other) const { return sig_iter == other.sig_iter; }
// 	NetlistConeInputsIter &operator++()
// 	{
// 		do {
// 			++sig_iter;
// 			if (sig_iter->empty()) {
// 				return *this;
// 			}
// 		} while (has_driver_cell(sig_iter));

// 		return *this;
// 	}
// };

// struct NetlistConeInputsIterable {
// 	const Netlist &net;
// 	RTLIL::SigBit sig;

// 	NetlistConeInputsIterable(const Netlist &net, RTLIL::SigBit sig) : net(net), sig(sig) {}

// 	NetlistConeInputsIter begin() { return NetlistConeInputsIter(net, sig); }
// 	NetlistConeInputsIter end() { return NetlistConeInputsIter(net); }
// };
} // namespace detail

detail::NetlistConeWireIterable cone(const Netlist &net, RTLIL::SigBit sig) { return detail::NetlistConeWireIterable(net, net.sigmap(sig)); }

// detail::NetlistConeInputsIterable cone_inputs(RTLIL::SigBit sig) { return NetlistConeInputsIterable(this, &sig); }
detail::NetlistConeCellIterable cell_cone(const Netlist &net, RTLIL::SigBit sig) { return detail::NetlistConeCellIterable(net, net.sigmap(sig)); }

YOSYS_NAMESPACE_END

#endif
