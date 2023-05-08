#include <eosio/asset.hpp>
#include <eosio/eosio.hpp>
#include <eosio/system.hpp>
#include <math.h>
#include <string>

inline uint64_t now() { return (uint64_t)(eosio::current_time_point().sec_since_epoch()); }

struct [[eosio::table("cfgtokens"), eosio::contract("tokenbridge")]] cfgtokens {
    eosio::name name;

    auto primary_key() const { return name.value; }
};

using cfgtokens_table = eosio::multi_index<"cfgtokens"_n, cfgtokens>;

struct [[eosio::table("orderbook"), eosio::contract("tokenbridge")]] orderbook {
    uint64_t     id;
    eosio::name  owner;
    eosio::asset token;
    std::string  rebus_address;
    uint64_t     status; // mapped because of index efficiency; 0 means new, 1 means in progress, 2 means completed
    uint64_t     created_at;
    uint64_t     updated_at;

    auto     primary_key() const { return id; }
    uint64_t owner_key() const { return owner.value; }
    uint64_t status_key() const { return status; }
};

using orderbook_table = eosio::multi_index<
    "orderbook"_n, orderbook, eosio::indexed_by<"owner"_n, eosio::const_mem_fun<orderbook, uint64_t, &orderbook::owner_key>>,
    eosio::indexed_by<"status"_n, eosio::const_mem_fun<orderbook, uint64_t, &orderbook::status_key>>>;

// The contract
class tokenbridge : eosio::contract {
  public:
    // Use contract's constructor
    using contract::contract;

    [[eosio::action("addcfgtoken")]] void addcfgtoken(const eosio::name symbol) {
        require_auth(get_self());

        cfgtokens_table _cfgtokens{get_self(), 0};

        _cfgtokens.emplace(get_self(), [&](cfgtokens& row) { row.name = symbol; });
    }

    [[eosio::action("delcfgtoken")]] void delcfgtoken(const eosio::name symbol) {
        require_auth(get_self());

        cfgtokens_table _cfgtokens{get_self(), 0};

        auto itr = _cfgtokens.find(symbol.value);
        eosio::check(itr != _cfgtokens.end(), "Symbol does not exist");
        _cfgtokens.erase(itr);
    }

    [[eosio::action("markcmpleted")]] void markcompleted(const uint64_t order_id) {
        require_auth(get_self());

        orderbook_table _orders{get_self(), 0};

        auto itr = _orders.find(order_id);
        eosio::check(itr != _orders.end(), "Order with that id does not exist");

        _orders.modify(itr, get_self(), [&](orderbook& row) {
            row.status     = 2;
            row.updated_at = now();
        });
    }

    [[eosio::action("markprogress")]] void markinprogress(const uint64_t order_id) {
        require_auth(get_self());

        orderbook_table _orders{get_self(), 0};

        auto itr = _orders.find(order_id);
        eosio::check(itr != _orders.end(), "Order with that id does not exist");

        _orders.modify(itr, get_self(), [&](orderbook& row) {
            row.status     = 1;
            row.updated_at = now();
        });
    }

    [[eosio::action("logorder")]] void logorder(const uint64_t order_id, const eosio::name owner, const std::string rebus_address, const eosio::asset token) {
        require_auth(get_self());
    }

    using logorder_action = eosio::action_wrapper<"logorder"_n, &tokenbridge::logorder>;

    [[eosio::action("placeorder")]] void placeorder(const eosio::name owner, const std::string rebus_address, const eosio::asset token) {
        require_auth(get_self());

        orderbook_table _orders{get_self(), 0};
        auto            creation_time = now();

        _orders.emplace(get_self(), [&](orderbook& row) {
            row.id            = _orders.available_primary_key();
            row.owner         = owner;
            row.token         = token;
            row.rebus_address = rebus_address;
            row.status        = 0;
            row.created_at    = creation_time;
            row.updated_at    = creation_time;
        });
    }
    using placeorder_action = eosio::action_wrapper<"placeorder"_n, &tokenbridge::placeorder>;

    [[eosio::on_notify("eosio.token::transfer")]] void
    transfer_notifier(const eosio::name hodler, const eosio::name to, const eosio::asset token, const std::string memo) {
        cfgtokens_table _cfgtokens{get_self(), 0};

        if (hodler == get_self() || to != get_self()) {
            return;
        }

        require_recipient(hodler);

        std::string token_symbol = token.symbol.code().to_string();

        for (int i = 0; i < token_symbol.length(); i++) {
            token_symbol[i] = std::tolower(token_symbol[i]);
        }

        eosio::check(token.symbol.is_valid(), "invalid deposit symbol name");
        eosio::check(token.is_valid(), "invalid deposit");
        eosio::check(token.amount > 0, "deposit amount must be positive");

        auto itr = _cfgtokens.find(eosio::name(token_symbol).value);
        if (itr == _cfgtokens.end()) {
            eosio::print_f("token with code: %d not in list, skip bridging", token_symbol);
            return;
        }

        std::string action = memo.substr(0, memo.find("|"));

        if (action != "bridge") {
            return;
        }

        std::string rebus_address = memo.substr(memo.find("|") + 1, size(memo));

        tokenbridge::placeorder_action placeorder(get_self(), {get_self(), "active"_n});
        placeorder.send(hodler, rebus_address, token);
    }
};
