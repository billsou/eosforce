/**
 *  @file
 *  @copyright defined in eos/LICENSE
 */
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/contract_table_objects.hpp>

#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <eosio/chain/apply_context.hpp>
#include <eosio/chain/transaction.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain/memory_db.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/permission_object.hpp>
#include <eosio/chain/permission_link_object.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/contract_types.hpp>
#include <eosio/chain/producer_object.hpp>

#include <eosio/chain/wasm_interface.hpp>
#include <eosio/chain/abi_serializer.hpp>

#include <eosio/chain/config_on_chain.hpp>

#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/resource_limits.hpp>
// #include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/txfee_manager.hpp>

namespace eosio { namespace chain {



uint128_t transaction_id_to_sender_id( const transaction_id_type& tid ) {
   fc::uint128_t _id(tid._hash[3], tid._hash[2]);
   return (unsigned __int128)_id;
}

void validate_authority_precondition( const apply_context& context, const authority& auth ) {
   for(const auto& a : auth.accounts) {
      auto* acct = context.db.find<account_object, by_name>(a.permission.actor);
      EOS_ASSERT( acct != nullptr, action_validate_exception,
                  "account '${account}' does not exist",
                  ("account", a.permission.actor)
                );

      if( a.permission.permission == config::owner_name || a.permission.permission == config::active_name )
         continue; // account was already checked to exist, so its owner and active permissions should exist

      if( a.permission.permission == config::eosio_code_name ) // virtual eosio.code permission does not really exist but is allowed
         continue;

      try {
         context.control.get_authorization_manager().get_permission({a.permission.actor, a.permission.permission});
      } catch( const permission_query_exception& ) {
         EOS_THROW( action_validate_exception,
                    "permission '${perm}' does not exist",
                    ("perm", a.permission)
                  );
      }
   }

   if( context.trx_context.enforce_whiteblacklist && context.control.is_producing_block() ) {
      for( const auto& p : auth.keys ) {
         context.control.check_key_list( p.key );
      }
   }
}

/**
 *  This method is called assuming precondition_system_newaccount succeeds a
 */
void apply_eosio_newaccount(apply_context& context) {
   auto create = context.act.data_as<newaccount>();
   try {
   context.require_authorization(create.creator);
//   context.require_write_lock( config::eosio_auth_scope );
   auto& authorization = context.control.get_mutable_authorization_manager();

   EOS_ASSERT( validate(create.owner), action_validate_exception, "Invalid owner authority");
   EOS_ASSERT( validate(create.active), action_validate_exception, "Invalid active authority");

   auto& db = context.db;

   auto name_str = name(create.name).to_string();

   EOS_ASSERT( !create.name.empty(), action_validate_exception, "account name cannot be empty" );
   EOS_ASSERT( name_str.size() <= 12, action_validate_exception, "account names can only be 12 chars long" );

   // Check if the creator is privileged
   const auto &creator = db.get<account_object, by_name>(create.creator);
   EOS_ASSERT(!creator.privileged, action_validate_exception, "not support privileged accounts");


   if(    is_func_has_open(context.control, config::func_typ::onfee_action)
       && (create.creator != config::system_account_name)
       && (create.creator != config::producers_account_name)
       && (create.creator != config::chain_config_name) ) {
      EOS_ASSERT( name_str.find( "force." ) != 0, action_validate_exception,
                  "only privileged accounts can have names that start with 'force.'" );
   }

   auto existing_account = db.find<account_object, by_name>(create.name);
   EOS_ASSERT(existing_account == nullptr, account_name_exists_exception,
              "Cannot create account named ${name}, as that name is already taken",
              ("name", create.name));

   const auto& new_account = db.create<account_object>([&](auto& a) {
      a.name = create.name;
      a.creation_date = context.control.pending_block_time();
   });

   db.create<account_sequence_object>([&](auto& a) {
      a.name = create.name;
   });

   // accounts_table
   memory_db(db).insert(
         config::system_account_name, config::system_account_name, N(accounts),
         create.name,
         memory_db::account_info{create.name, asset(0)});

   for( const auto& auth : { create.owner, create.active } ){
      validate_authority_precondition( context, auth );
   }

   const auto& owner_permission  = authorization.create_permission( create.name, config::owner_name, 0,
                                                                    std::move(create.owner) );
   const auto& active_permission = authorization.create_permission( create.name, config::active_name, owner_permission.id,
                                                                    std::move(create.active) );

   context.control.get_mutable_resource_limits_manager().initialize_account(create.name);

   int64_t ram_delta = config::overhead_per_account_ram_bytes;
   ram_delta += 2*config::billable_size_v<permission_object>;
   ram_delta += owner_permission.auth.get_billable_size();
   ram_delta += active_permission.auth.get_billable_size();

   context.add_ram_usage(create.name, ram_delta);

} FC_CAPTURE_AND_RETHROW( (create) ) }

void apply_eosio_setcode(apply_context& context) {
   const auto& cfg = context.control.get_global_properties().configuration;

   auto& db = context.db;
   auto  act = context.act.data_as<setcode>();
   context.require_authorization(act.account);

   EOS_ASSERT( act.vmtype == 0, invalid_contract_vm_type, "code should be 0" );
   EOS_ASSERT( act.vmversion == 0, invalid_contract_vm_version, "version should be 0" );

   fc::sha256 code_id; /// default ID == 0

   if( act.code.size() > 0 ) {
     code_id = fc::sha256::hash( act.code.data(), (uint32_t)act.code.size() );
     wasm_interface::validate(context.control, act.code);
   }

   const auto& account = db.get<account_object,by_name>(act.account);

   int64_t code_size = (int64_t)act.code.size();
   int64_t old_size  = (int64_t)account.code.size() * config::setcode_ram_bytes_multiplier;
   int64_t new_size  = code_size * config::setcode_ram_bytes_multiplier;

   EOS_ASSERT( account.code_version != code_id, set_exact_code, "contract is already running this version of code" );

   db.modify( account, [&]( auto& a ) {
      /** TODO: consider whether a microsecond level local timestamp is sufficient to detect code version changes*/
      // TODO: update setcode message to include the hash, then validate it in validate
      a.last_code_update = context.control.pending_block_time();
      a.code_version = code_id;
      if ( code_size > 0 ) {
         a.code.assign(act.code.data(), code_size);
      } else {
         a.code.resize(0);
      }
   });

   const auto& account_sequence = db.get<account_sequence_object, by_name>(act.account);
   db.modify( account_sequence, [&]( auto& aso ) {
      aso.code_sequence += 1;
   });

   if (new_size != old_size) {
      context.add_ram_usage( act.account, new_size - old_size );
   }
}

// setfee just for test imp contracts
void apply_eosio_setfee(apply_context& context) {
   auto &db = context.db;
   auto act = context.act.data_as<setfee>();

   // need force.test
   // TODO add power Invalid block number
   // idump((context.act.authorization));

   /*
   * About Fee
   *
   * fee come from three mode:
   *  - native, set in cpp
   *  - set by eosio
   *  - set by user
   *
   * and res limit can set a value or zero,
   * all of this will make diff mode to calc res limit,
   * support 1.0 EOS is for `C` cpu, `N` net and `R` ram,
   * and cost fee by `f` EOS and extfee by `F` EOS
   *
   * then can give:
   *  - native and no setfee : use native fee and unlimit res use
   *  - eosio set fee {f, (c,n,r)}
   *      (cpu_limit, net_limit, ram_limit) == (c + F*C, n + F*N, r + F*R)
   *  - eosio set fee {f, (0,0,0)}
   *      (cpu_limit, net_limit, ram_limit) == ((f+F)*C, (f+F)*N, (f+F)*R)
   *  - user set fee {f, (0,0,0)}, user cannot set fee by c>0||n>0||r>0
   *      (cpu_limit, net_limit, ram_limit) == ((f+F)*C, (f+F)*N, (f+F)*R)
   *
   *  so it can be check by:
   *  if no setfee
   *       if no native -> err
   *       if native -> use native and unlimit res use
   *  else
   *       if res limit is (0,0,0) -> limit res by ((f+F)*C, (f+F)*N, (f+F)*R)
   *       if res limit is (c,n,r) -> (c + F*C, n + F*N, r + F*R)
   *
   *  at the same time, eosio can set res limit > (0,0,0) and user cannot
   *
   * */

   if(   ( act.cpu_limit == 0 )
      && ( act.net_limit == 0 )
      && ( act.ram_limit == 0 ) ) {
      if(!context.has_authorization(N(force.test))) {
         context.require_authorization(act.account);
      }
   } else {
      context.require_authorization(N(force.test));
   }

   // by keep for main net so use get_amount
   EOS_ASSERT(act.fee.get_amount() > 0,
         invalid_action_args_exception, "fee can not be zero");

   // a max will need check
   EOS_ASSERT(act.fee.get_amount() <= (200 * 10000),
         invalid_action_args_exception, "fee can not too mush, more then 200.0000 EOS");

   // warning
   const auto key = boost::make_tuple(act.account, act.action);
   auto fee_old = db.find<chain::action_fee_object, chain::by_action_name>(key);
   if(fee_old == nullptr) {
      //dlog("need create fee");
      db.create<chain::action_fee_object>([&]( auto& fee_obj ) {
         fee_obj.account = act.account;
         fee_obj.message_type = act.action;
         fee_obj.fee = act.fee;
         fee_obj.cpu_limit = act.cpu_limit;
         fee_obj.net_limit = act.net_limit;
         fee_obj.ram_limit = act.ram_limit;
      });
   } else {
      db.modify<chain::action_fee_object>( *fee_old, [&]( auto& fee_obj ) {
         fee_obj.fee = act.fee;
         fee_obj.cpu_limit = act.cpu_limit;
         fee_obj.net_limit = act.net_limit;
         fee_obj.ram_limit = act.ram_limit;
      });
   }
}

void apply_eosio_setabi(apply_context& context) {
   auto& db  = context.db;
   auto  act = context.act.data_as<setabi>();

   context.require_authorization(act.account);

   const auto& account = db.get<account_object,by_name>(act.account);

   int64_t abi_size = act.abi.size();

   int64_t old_size = (int64_t)account.abi.size();
   int64_t new_size = abi_size;

   db.modify( account, [&]( auto& a ) {
      if (abi_size > 0) {
         a.abi.assign(act.abi.data(), abi_size);
      } else {
         a.abi.resize(0);
      }
   });

   const auto& account_sequence = db.get<account_sequence_object, by_name>(act.account);
   db.modify( account_sequence, [&]( auto& aso ) {
      aso.abi_sequence += 1;
   });

   if (new_size != old_size) {
      context.add_ram_usage( act.account, new_size - old_size );
   }
}

void apply_eosio_updateauth(apply_context& context) {

   auto update = context.act.data_as<updateauth>();
   context.require_authorization(update.account); // only here to mark the single authority on this action as used

   auto& authorization = context.control.get_mutable_authorization_manager();
   auto& db = context.db;

   EOS_ASSERT(!update.permission.empty(), action_validate_exception, "Cannot create authority with empty name");
   EOS_ASSERT( update.permission.to_string().find( "eosio." ) != 0, action_validate_exception,
               "Permission names that start with 'eosio.' are reserved" );
   EOS_ASSERT( update.permission.to_string().find( "force." ) != 0, action_validate_exception,
               "Permission names that start with 'force.' are reserved" );
   EOS_ASSERT(update.permission != update.parent, action_validate_exception, "Cannot set an authority as its own parent");
   db.get<account_object, by_name>(update.account);
   EOS_ASSERT(validate(update.auth), action_validate_exception,
              "Invalid authority: ${auth}", ("auth", update.auth));
   if( update.permission == config::active_name )
      EOS_ASSERT(update.parent == config::owner_name, action_validate_exception, "Cannot change active authority's parent from owner", ("update.parent", update.parent) );
   if (update.permission == config::owner_name)
      EOS_ASSERT(update.parent.empty(), action_validate_exception, "Cannot change owner authority's parent");
   else
      EOS_ASSERT(!update.parent.empty(), action_validate_exception, "Only owner permission can have empty parent" );

   if( update.auth.waits.size() > 0 ) {
      auto max_delay = context.control.get_global_properties().configuration.max_transaction_delay;
      EOS_ASSERT( update.auth.waits.back().wait_sec <= max_delay, action_validate_exception,
                  "Cannot set delay longer than max_transacton_delay, which is ${max_delay} seconds",
                  ("max_delay", max_delay) );
   }

   validate_authority_precondition(context, update.auth);



   auto permission = authorization.find_permission({update.account, update.permission});

   // If a parent_id of 0 is going to be used to indicate the absence of a parent, then we need to make sure that the chain
   // initializes permission_index with a dummy object that reserves the id of 0.
   authorization_manager::permission_id_type parent_id = 0;
   if( update.permission != config::owner_name ) {
      auto& parent = authorization.get_permission({update.account, update.parent});
      parent_id = parent.id;
   }

   if( permission ) {
      EOS_ASSERT(parent_id == permission->parent, action_validate_exception,
                 "Changing parent authority is not currently supported");


      int64_t old_size = (int64_t)(config::billable_size_v<permission_object> + permission->auth.get_billable_size());

      authorization.modify_permission( *permission, update.auth );

      int64_t new_size = (int64_t)(config::billable_size_v<permission_object> + permission->auth.get_billable_size());

      context.add_ram_usage( permission->owner, new_size - old_size );
   } else {
      const auto& p = authorization.create_permission( update.account, update.permission, parent_id, update.auth );

      int64_t new_size = (int64_t)(config::billable_size_v<permission_object> + p.auth.get_billable_size());

      context.add_ram_usage( update.account, new_size );
   }
}

void apply_eosio_deleteauth(apply_context& context) {
//   context.require_write_lock( config::eosio_auth_scope );

   auto remove = context.act.data_as<deleteauth>();
   context.require_authorization(remove.account); // only here to mark the single authority on this action as used

   EOS_ASSERT(remove.permission != config::active_name, action_validate_exception, "Cannot delete active authority");
   EOS_ASSERT(remove.permission != config::owner_name, action_validate_exception, "Cannot delete owner authority");

   auto& authorization = context.control.get_mutable_authorization_manager();
   auto& db = context.db;



   { // Check for links to this permission
      const auto& index = db.get_index<permission_link_index, by_permission_name>();
      auto range = index.equal_range(boost::make_tuple(remove.account, remove.permission));
      EOS_ASSERT(range.first == range.second, action_validate_exception,
                 "Cannot delete a linked authority. Unlink the authority first. This authority is linked to ${code}::${type}.",
                 ("code", string(range.first->code))("type", string(range.first->message_type)));
   }

   const auto& permission = authorization.get_permission({remove.account, remove.permission});
   int64_t old_size = config::billable_size_v<permission_object> + permission.auth.get_billable_size();

   authorization.remove_permission( permission );

   context.add_ram_usage( remove.account, -old_size );

}

void apply_eosio_linkauth(apply_context& context) {
//   context.require_write_lock( config::eosio_auth_scope );

   auto requirement = context.act.data_as<linkauth>();
   try {
      EOS_ASSERT(!requirement.requirement.empty(), action_validate_exception, "Required permission cannot be empty");

      context.require_authorization(requirement.account); // only here to mark the single authority on this action as used

      auto& db = context.db;
      const auto *account = db.find<account_object, by_name>(requirement.account);
      EOS_ASSERT(account != nullptr, account_query_exception,
                 "Failed to retrieve account: ${account}", ("account", requirement.account)); // Redundant?
      const auto *code = db.find<account_object, by_name>(requirement.code);
      EOS_ASSERT(code != nullptr, account_query_exception,
                 "Failed to retrieve code for account: ${account}", ("account", requirement.code));
      if( requirement.requirement != config::eosio_any_name ) {
         const auto *permission = db.find<permission_object, by_name>(requirement.requirement);
         EOS_ASSERT(permission != nullptr, permission_query_exception,
                    "Failed to retrieve permission: ${permission}", ("permission", requirement.requirement));
      }

      auto link_key = boost::make_tuple(requirement.account, requirement.code, requirement.type);
      auto link = db.find<permission_link_object, by_action_name>(link_key);

      if( link ) {
         EOS_ASSERT(link->required_permission != requirement.requirement, action_validate_exception,
                    "Attempting to update required authority, but new requirement is same as old");
         db.modify(*link, [requirement = requirement.requirement](permission_link_object& link) {
             link.required_permission = requirement;
         });
      } else {
         const auto& l =  db.create<permission_link_object>([&requirement](permission_link_object& link) {
            link.account = requirement.account;
            link.code = requirement.code;
            link.message_type = requirement.type;
            link.required_permission = requirement.requirement;
         });

         context.add_ram_usage(
            l.account,
            (int64_t)(config::billable_size_v<permission_link_object>)
         );
      }

  } FC_CAPTURE_AND_RETHROW((requirement))
}

void apply_eosio_unlinkauth(apply_context& context) {
//   context.require_write_lock( config::eosio_auth_scope );

   auto& db = context.db;
   auto unlink = context.act.data_as<unlinkauth>();

   context.require_authorization(unlink.account); // only here to mark the single authority on this action as used

   auto link_key = boost::make_tuple(unlink.account, unlink.code, unlink.type);
   auto link = db.find<permission_link_object, by_action_name>(link_key);
   EOS_ASSERT(link != nullptr, action_validate_exception, "Attempting to unlink authority, but no link found");
   context.add_ram_usage(
      link->account,
      -(int64_t)(config::billable_size_v<permission_link_object>)
   );

   db.remove(*link);
}

void apply_eosio_canceldelay(apply_context& context) {
   auto cancel = context.act.data_as<canceldelay>();
   context.require_authorization(cancel.canceling_auth.actor); // only here to mark the single authority on this action as used

   const auto& trx_id = cancel.trx_id;

   context.cancel_deferred_transaction(transaction_id_to_sender_id(trx_id), account_name());
}

void apply_eosio_setconfig(apply_context& context) {
   auto cfg_data = context.act.data_as<setconfig>();
   if(    !context.has_authorization(config::chain_config_name)
       && !context.has_authorization(config::system_account_name)
       && !context.has_authorization(config::producers_account_name) ) {
      EOS_THROW(missing_auth_exception, "setconfig need auth by eosio.prods");
      return;
   } 
   
   set_config_on_chain(context.db, cfg_data);
}

void apply_eosio_onfee( apply_context& context ) {
   const auto data = context.act.data_as<onfee>();
   const auto& fee = data.fee;

   // fee is just can push by system auto, so it need less check
   // need actor authorization
   // context.require_authorization(data.actor);

   // accounts_table
   auto acnts_tbl = native_multi_index<N(accounts), memory_db::account_info>{
         context, config::system_account_name, config::system_account_name
   };
   memory_db::account_info account_info_data;
   acnts_tbl.get(data.actor, account_info_data, "account is not found in accounts table");
   eosio_contract_assert(fee <= account_info_data.available, "overdrawn available balance");

   acnts_tbl.modify(acnts_tbl.find_itr(data.actor), account_info_data, 0,
                    [fee]( memory_db::account_info& a ) {
                       a.available -= fee;
                    });

   if( data.bpname != name{} ) {
      // bps_table
      auto bps_tbl = native_multi_index<N(bps), memory_db::bp_info>{
            context, config::system_account_name, config::system_account_name
      };
      memory_db::bp_info bp_info_data;
      bps_tbl.get(data.bpname, bp_info_data, "bpname is not registered");
      bps_tbl.modify(bps_tbl.find_itr(data.bpname), bp_info_data, 0, [fee](memory_db::bp_info& a) {
         a.rewards_pool += fee;
      });
   }
}

void apply_eosio_voteagefee( apply_context& context ) {
   const auto data = context.act.data_as<voteagefee>();
   const auto& fee = data.fee;

   // fee is just can push by system auto, so it need less check
   // need actor authorization
   // context.require_authorization(data.actor);

   // accounts_table
   auto acnts_tbl = native_multi_index<N(accounts), memory_db::account_info>{
         context, config::system_account_name, config::system_account_name
   };
   memory_db::account_info account_info_data;
   acnts_tbl.get(data.actor, account_info_data, "account is not found in accounts table");

   // bps_table
   auto bps_tbl = native_multi_index<N(bps), memory_db::bp_info>{
         context, config::system_account_name, config::system_account_name
   };
   memory_db::bp_info bp_info_data;
   bps_tbl.get(data.bpname, bp_info_data, "bpname is not registered");
      
   const auto voteage = fee.get_amount() * get_num_config_on_chain( context.db, config::res_typ::votage_ratio, 10000 ) / 10000;
   const auto curr_block_num = context.control.head_block_num();
   const auto newest_total_voteage =
      bp_info_data.total_voteage + bp_info_data.total_staked * (curr_block_num - bp_info_data.voteage_update_height);

   /*ilog("apply_eosio_onfee: fee voteage=${voteage}, bp total_voteage=${total_voteage}, bp total_staked=${total_staked}, curr_block_num=${curr_block_num}, bp voteage_update_height=${voteage_update_height}, newest_total_voteage=${newest_total_voteage}", 
         ("voteage", voteage)
         ("total_voteage", bp_info_data.total_voteage)
         ("total_staked", bp_info_data.total_staked) 
         ("curr_block_num", curr_block_num)
         ("voteage_update_height", bp_info_data.voteage_update_height) 
         ("newest_total_voteage", newest_total_voteage));*/
   
   auto votes_tbl = native_multi_index<N(votes), memory_db::vote_info>{
         context, config::system_account_name, data.actor
   };
   memory_db::vote_info vote_info_data;
   votes_tbl.get(data.bpname, vote_info_data, "voter have not add votes to the the producer yet");
   
   const auto newest_voteage = vote_info_data.voteage + (vote_info_data.staked.get_amount() / 10000) * (curr_block_num - vote_info_data.voteage_update_height);
   
   /*ilog("apply_eosio_onfee: voter voteage=${voteage}, voter vote=${vote}, voter voteage_update_height=${voteage_update_height}, voter newest_voteage=${newest_voteage}", 
      ("voteage", vote_info_data.voteage) 
      ("vote", vote_info_data.staked.get_amount() / 10000)
      ("voteage_update_height", vote_info_data.voteage_update_height) 
      ("newest_voteage", newest_voteage));*/
   eosio_contract_assert(voteage > 0 && voteage <= newest_voteage, "voteage must be greater than zero and have sufficient voteage!");
   
   votes_tbl.modify(votes_tbl.find_itr(data.bpname), vote_info_data, 0, [&]( memory_db::vote_info& v ) {
      v.voteage = newest_voteage - voteage;
      v.voteage_update_height = curr_block_num;
   });
   
   bps_tbl.modify(bps_tbl.find_itr(data.bpname), bp_info_data, 0, [&]( memory_db::bp_info& b ) {
      b.total_voteage = static_cast<int64_t>(newest_total_voteage - voteage);
      b.voteage_update_height = curr_block_num;
   });
}

} } // namespace eosio::chain
