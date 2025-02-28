#include <eosio/chain/controller.hpp>
#include <eosio/chain/transaction_context.hpp>

#include <eosio/chain/block_log.hpp>
#include <eosio/chain/fork_database.hpp>
#include <eosio/chain/exceptions.hpp>

#include <eosio/chain/account_object.hpp>
#include <eosio/chain/memory_db.hpp>
#include <eosio/chain/block_summary_object.hpp>
#include <eosio/chain/eosio_contract.hpp>
#include <eosio/chain/global_property_object.hpp>
#include <eosio/chain/contract_table_objects.hpp>
#include <eosio/chain/generated_transaction_object.hpp>
#include <eosio/chain/transaction_object.hpp>
#include <eosio/chain/reversible_block_object.hpp>

#include <eosio/chain/authorization_manager.hpp>
#include <eosio/chain/txfee_manager.hpp>
#include <eosio/chain/config_on_chain.hpp>
#include <eosio/chain/resource_limits.hpp>
#include <eosio/chain/resource_limits_private.hpp>
#include <eosio/chain/config.hpp>
#include <eosio/chain/chain_snapshot.hpp>
#include <eosio/chain/thread_utils.hpp>

#include <chainbase/chainbase.hpp>
#include <fc/io/json.hpp>
#include <fc/scoped_exit.hpp>
#include <fc/variant_object.hpp>

#include <eosio/chain/eosio_contract.hpp>
#include <set>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>


namespace eosio { namespace chain {

using resource_limits::resource_limits_manager;

using controller_index_set = index_set<
   account_index,
   account_sequence_index,
   global_property_multi_index,
   dynamic_global_property_multi_index,
   block_summary_multi_index,
   transaction_multi_index,
   generated_transaction_multi_index,
   table_id_multi_index,
   action_fee_object_index,
   config_data_object_index
>;

using contract_database_index_set = index_set<
   key_value_index,
   index64_index,
   index128_index,
   index256_index,
   index_double_index,
   index_long_double_index
>;

class maybe_session {
   public:
      maybe_session() = default;

      maybe_session( maybe_session&& other)
      :_session(move(other._session))
      {
      }

      explicit maybe_session(database& db) {
         _session = db.start_undo_session(true);
      }

      maybe_session(const maybe_session&) = delete;

      void squash() {
         if (_session)
            _session->squash();
      }

      void undo() {
         if (_session)
            _session->undo();
      }

      void push() {
         if (_session)
            _session->push();
      }

      maybe_session& operator = ( maybe_session&& mv ) {
         if (mv._session) {
            _session = move(*mv._session);
            mv._session.reset();
         } else {
            _session.reset();
         }

         return *this;
      };

   private:
      optional<database::session>     _session;
};

struct pending_state {
   pending_state( maybe_session&& s )
   :_db_session( move(s) ){}

   maybe_session                      _db_session;

   block_state_ptr                    _pending_block_state;

   vector<action_receipt>             _actions;

   controller::block_status           _block_status = controller::block_status::incomplete;

   optional<block_id_type>            _producer_block_id;

   void push() {
      _db_session.push();
   }
};

struct controller_impl {
   controller&                    self;
   chainbase::database            db;
   chainbase::database            reversible_blocks; ///< a special database to persist blocks that have successfully been applied but are still reversible
   block_log                      blog;
   optional<pending_state>        pending;
   block_state_ptr                head;
   fork_database                  fork_db;
   wasm_interface                 wasmif;
   resource_limits_manager        resource_limits;
   authorization_manager          authorization;
   txfee_manager                  txfee;
   controller::config             conf;
   chain_id_type                  chain_id;
   bool                           replaying= false;
   optional<fc::time_point>       replay_head_time;
   db_read_mode                   read_mode = db_read_mode::SPECULATIVE;
   bool                           in_trx_requiring_checks = false; ///< if true, checks that are normally skipped on replay (e.g. auth checks) cannot be skipped
   optional<fc::microseconds>     subjective_cpu_leeway;
   bool                           trusted_producer_light_validation = false;
   uint32_t                       snapshot_head_block = 0;
   boost::asio::thread_pool       thread_pool;

   typedef pair<scope_name,action_name>                   handler_key;
   map< account_name, map<handler_key, apply_handler> >   apply_handlers;

   /**
    *  Transactions that were undone by pop_block or abort_block, transactions
    *  are removed from this list if they are re-applied in other blocks. Producers
    *  can query this list when scheduling new transactions into blocks.
    */
   unapplied_transactions_type     unapplied_transactions;

   void pop_block() {
      auto prev = fork_db.get_block( head->header.previous );
      EOS_ASSERT( prev, block_validate_exception, "attempt to pop beyond last irreversible block" );

      if( const auto* b = reversible_blocks.find<reversible_block_object,by_num>(head->block_num) )
      {
         reversible_blocks.remove( *b );
      }

      if ( read_mode == db_read_mode::SPECULATIVE ) {
         EOS_ASSERT( head->block, block_validate_exception, "attempting to pop a block that was sparsely loaded from a snapshot");
         for( const auto& t : head->trxs )
            unapplied_transactions[t->signed_id] = t;
      }
      head = prev;
      db.undo();

   }


   void set_apply_handler( account_name receiver, account_name contract, action_name action, apply_handler v ) {
      apply_handlers[receiver][make_pair(contract,action)] = v;
   }

   controller_impl( const controller::config& cfg, controller& s  )
   :self(s),
    db( cfg.state_dir,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.state_size ),
    reversible_blocks( cfg.blocks_dir/config::reversible_blocks_dir_name,
        cfg.read_only ? database::read_only : database::read_write,
        cfg.reversible_cache_size ),
    blog( cfg.blocks_dir ),
    fork_db( cfg.state_dir ),
    wasmif( cfg.wasm_runtime ),
    resource_limits( db ),
    authorization( s, db ),
    txfee(),
    conf( cfg ),
    chain_id( cfg.genesis.compute_chain_id() ),
    read_mode( cfg.read_mode ),
    thread_pool( cfg.thread_pool_size )
   {

#define SET_APP_HANDLER( receiver, contract, action) \
   set_apply_handler( #receiver, #contract, #action, &BOOST_PP_CAT(apply_, BOOST_PP_CAT(contract, BOOST_PP_CAT(_,action) ) ) )

   // add a asset if system account is change, if it changed, next SET_APP_HANDLER need also change
   BOOST_STATIC_ASSERT(N(eosio)       == config::system_account_name);
   BOOST_STATIC_ASSERT(N(eosio.token) == config::token_account_name);

   SET_APP_HANDLER( eosio, eosio, newaccount );
   SET_APP_HANDLER( eosio, eosio, setcode );
   SET_APP_HANDLER( eosio, eosio, setconfig );
   SET_APP_HANDLER( eosio, eosio, setfee );
   SET_APP_HANDLER( eosio, eosio, setabi );
   SET_APP_HANDLER( eosio, eosio, updateauth );
   SET_APP_HANDLER( eosio, eosio, deleteauth );
   SET_APP_HANDLER( eosio, eosio, linkauth );
   SET_APP_HANDLER( eosio, eosio, unlinkauth );
/*
   SET_APP_HANDLER( eosio, eosio, postrecovery );
   SET_APP_HANDLER( eosio, eosio, passrecovery );
   SET_APP_HANDLER( eosio, eosio, vetorecovery );
*/

   SET_APP_HANDLER( eosio, eosio, canceldelay );
   SET_APP_HANDLER( eosio, eosio, onfee );
   SET_APP_HANDLER( eosio, eosio, voteagefee );

   fork_db.irreversible.connect( [&]( auto b ) {
                                 on_irreversible(b);
                                 });

   }

   /**
    *  Plugins / observers listening to signals emited (such as accepted_transaction) might trigger
    *  errors and throw exceptions. Unless those exceptions are caught it could impact consensus and/or
    *  cause a node to fork.
    *
    *  If it is ever desirable to let a signal handler bubble an exception out of this method
    *  a full audit of its uses needs to be undertaken.
    *
    */
   template<typename Signal, typename Arg>
   void emit( const Signal& s, Arg&& a ) {
      try {
        s(std::forward<Arg>(a));
      } catch (boost::interprocess::bad_alloc& e) {
         wlog( "bad alloc" );
         throw e;
      } catch ( controller_emit_signal_exception& e ) {
         wlog( "${details}", ("details", e.to_detail_string()) );
         throw e;
      } catch ( fc::exception& e ) {
         wlog( "${details}", ("details", e.to_detail_string()) );
      } catch ( ... ) {
         wlog( "signal handler threw exception" );
      }
   }

   void on_irreversible( const block_state_ptr& s ) {
      if( !blog.head() )
         blog.read_head();

      const auto& log_head = blog.head();
      bool append_to_blog = false;
      if (!log_head) {
         if (s->block) {
            EOS_ASSERT(s->block_num == blog.first_block_num(), block_log_exception, "block log has no blocks and is appending the wrong first block.  Expected ${expected}, but received: ${actual}",
                      ("expected", blog.first_block_num())("actual", s->block_num));
            append_to_blog = true;
         } else {
            EOS_ASSERT(s->block_num == blog.first_block_num() - 1, block_log_exception, "block log has no blocks and is not properly set up to start after the snapshot");
         }
      } else {
         auto lh_block_num = log_head->block_num();
         if (s->block_num > lh_block_num) {
            EOS_ASSERT(s->block_num - 1 == lh_block_num, unlinkable_block_exception, "unlinkable block", ("s->block_num", s->block_num)("lh_block_num", lh_block_num));
            EOS_ASSERT(s->block->previous == log_head->id(), unlinkable_block_exception, "irreversible doesn't link to block log head");
            append_to_blog = true;
         }
      }


      db.commit( s->block_num );

      if( append_to_blog ) {
         blog.append(s->block);
      }

      const auto& ubi = reversible_blocks.get_index<reversible_block_index,by_num>();
      auto objitr = ubi.begin();
      while( objitr != ubi.end() && objitr->blocknum <= s->block_num ) {
         reversible_blocks.remove( *objitr );
         objitr = ubi.begin();
      }

      // the "head" block when a snapshot is loaded is virtual and has no block data, all of its effects
      // should already have been loaded from the snapshot so, it cannot be applied
      if (s->block) {
         if (read_mode == db_read_mode::IRREVERSIBLE) {
            // when applying a snapshot, head may not be present
            // when not applying a snapshot, make sure this is the next block
            if (!head || s->block_num == head->block_num + 1) {
               apply_block(s->block, controller::block_status::complete);
               head = s;
            } else {
               // otherwise, assert the one odd case where initializing a chain
               // from genesis creates and applies the first block automatically.
               // when syncing from another chain, this is pushed in again
               EOS_ASSERT(!head || head->block_num == 1, block_validate_exception, "Attempting to re-apply an irreversible block that was not the implied genesis block");
            }

            fork_db.mark_in_current_chain(head, true);
            fork_db.set_validity(head, true);
         }
         emit(self.irreversible_block, s);
      }
   }

   void replay(std::function<bool()> shutdown) {
      auto blog_head = blog.read_head();
      auto blog_head_time = blog_head->timestamp.to_time_point();
      replaying = true;
      replay_head_time = blog_head_time;
      auto start_block_num = head->block_num + 1;
      ilog( "existing block log, attempting to replay from ${s} to ${n} blocks",
            ("s", start_block_num)("n", blog_head->block_num()) );

      auto start = fc::time_point::now();
      while( auto next = blog.read_block_by_num( head->block_num + 1 ) ) {
         replay_push_block( next, controller::block_status::irreversible );
         if( next->block_num() % 500 == 0 ) {
            ilog( "${n} of ${head}", ("n", next->block_num())("head", blog_head->block_num()) );
            if( shutdown() ) break;
         }
      }
      ilog( "${n} blocks replayed", ("n", head->block_num - start_block_num) );

      // if the irreversible log is played without undo sessions enabled, we need to sync the
      // revision ordinal to the appropriate expected value here.
      if( self.skip_db_sessions( controller::block_status::irreversible ) )
         db.set_revision(head->block_num);

      int rev = 0;
      while( auto obj = reversible_blocks.find<reversible_block_object,by_num>(head->block_num+1) ) {
         ++rev;
         replay_push_block( obj->get_block(), controller::block_status::validated );
      }

      ilog( "${n} reversible blocks replayed", ("n",rev) );
      auto end = fc::time_point::now();
      ilog( "replayed ${n} blocks in ${duration} seconds, ${mspb} ms/block",
            ("n", head->block_num - start_block_num)("duration", (end-start).count()/1000000)
            ("mspb", ((end-start).count()/1000.0)/(head->block_num-start_block_num)) );
      replaying = false;
      replay_head_time.reset();
   }

   void init(std::function<bool()> shutdown, const snapshot_reader_ptr& snapshot) {

      bool report_integrity_hash = !!snapshot;
      if (snapshot) {
         EOS_ASSERT( !head, fork_database_exception, "" );
         snapshot->validate();

         read_from_snapshot( snapshot );

         auto end = blog.read_head();
         if( !end ) {
            blog.reset( conf.genesis, signed_block_ptr(), head->block_num + 1 );
         } else if( end->block_num() > head->block_num ) {
            replay( shutdown );
         } else {
            EOS_ASSERT( end->block_num() == head->block_num, fork_database_exception,
                        "Block log is provided with snapshot but does not contain the head block from the snapshot" );
         }
      } else {
         if( !head ) {
            initialize_fork_db(); // set head to genesis state
         }

         auto end = blog.read_head();
         if( !end ) {
            blog.reset( conf.genesis, head->block );
         } else if( end->block_num() > head->block_num ) {
            replay( shutdown );
            report_integrity_hash = true;
         }
      }

      if( shutdown() ) return;

      const auto& ubi = reversible_blocks.get_index<reversible_block_index,by_num>();
      auto objitr = ubi.rbegin();
      if( objitr != ubi.rend() ) {
         EOS_ASSERT( objitr->blocknum == head->block_num, fork_database_exception,
                    "reversible block database is inconsistent with fork database, replay blockchain",
                    ("head",head->block_num)("unconfimed", objitr->blocknum)         );
      } else {
         auto end = blog.read_head();
         EOS_ASSERT( !end || end->block_num() == head->block_num, fork_database_exception,
                    "fork database exists but reversible block database does not, replay blockchain",
                    ("blog_head",end->block_num())("head",head->block_num)  );
      }

      EOS_ASSERT( db.revision() >= head->block_num, fork_database_exception, "fork database is inconsistent with shared memory",
                 ("db",db.revision())("head",head->block_num) );

      if( db.revision() > head->block_num ) {
         wlog( "warning: database revision (${db}) is greater than head block number (${head}), "
               "attempting to undo pending changes",
               ("db",db.revision())("head",head->block_num) );
      }
      while( db.revision() > head->block_num ) {
         db.undo();
      }

      if( report_integrity_hash ) {
         const auto hash = calculate_integrity_hash();
         ilog( "database initialized with hash: ${hash}", ("hash", hash) );
      }

   }

   ~controller_impl() {
      pending.reset();

      db.flush();
      reversible_blocks.flush();
   }

   void add_indices() {
      reversible_blocks.add_index<reversible_block_index>();

      controller_index_set::add_indices(db);
      contract_database_index_set::add_indices(db);

      //db.add_index<action_fee_object_index>();
      //db.add_index<config_data_object_index>();

      authorization.add_indices();
      resource_limits.add_indices();
   }

   void clear_all_undo() {
      // Rewind the database to the last irreversible block
      db.undo_all();
   }

   void initialize_schedule( producer_schedule_type& schedule ) {
      schedule.version = 0;
      schedule.producers.reserve(config::max_producers);
      for( const auto& producer : conf.genesis.initial_producer_list ) {
         schedule.producers.push_back({producer.name, producer.bpkey});
      }
   }

   void add_contract_tables_to_snapshot( const snapshot_writer_ptr& snapshot ) const {
      snapshot->write_section("contract_tables", [this]( auto& section ) {
         index_utils<table_id_multi_index>::walk(db, [this, &section]( const table_id_object& table_row ){
            // add a row for the table
            section.add_row(table_row, db);

            // followed by a size row and then N data rows for each type of table
            contract_database_index_set::walk_indices([this, &section, &table_row]( auto utils ) {
               using utils_t = decltype(utils);
               using value_t = typename decltype(utils)::index_t::value_type;
               using by_table_id = object_to_table_id_tag_t<value_t>;

               auto tid_key = boost::make_tuple(table_row.id);
               auto next_tid_key = boost::make_tuple(table_id_object::id_type(table_row.id._id + 1));

               unsigned_int size = utils_t::template size_range<by_table_id>(db, tid_key, next_tid_key);
               section.add_row(size, db);

               utils_t::template walk_range<by_table_id>(db, tid_key, next_tid_key, [this, &section]( const auto &row ) {
                  section.add_row(row, db);
               });
            });
         });
      });
   }

   void read_contract_tables_from_snapshot( const snapshot_reader_ptr& snapshot ) {
      snapshot->read_section("contract_tables", [this]( auto& section ) {
         bool more = !section.empty();
         while (more) {
            // read the row for the table
            table_id_object::id_type t_id;
            index_utils<table_id_multi_index>::create(db, [this, &section, &t_id](auto& row) {
               section.read_row(row, db);
               t_id = row.id;
            });

            // read the size and data rows for each type of table
            contract_database_index_set::walk_indices([this, &section, &t_id, &more](auto utils) {
               using utils_t = decltype(utils);

               unsigned_int size;
               more = section.read_row(size, db);

               for (size_t idx = 0; idx < size.value; idx++) {
                  utils_t::create(db, [this, &section, &more, &t_id](auto& row) {
                     row.t_id = t_id;
                     more = section.read_row(row, db);
                  });
               }
            });
         }
      });
   }

   void add_to_snapshot( const snapshot_writer_ptr& snapshot ) const {
      snapshot->write_section<chain_snapshot_header>([this]( auto &section ){
         section.add_row(chain_snapshot_header(), db);
      });

      snapshot->write_section<genesis_state>([this]( auto &section ){
         section.add_row(conf.genesis, db);
      });

      snapshot->write_section<block_state>([this]( auto &section ){
         section.template add_row<block_header_state>(*fork_db.head(), db);
      });

      controller_index_set::walk_indices([this, &snapshot]( auto utils ){
         using value_t = typename decltype(utils)::index_t::value_type;

         // skip the table_id_object as its inlined with contract tables section
         if (std::is_same<value_t, table_id_object>::value) {
            return;
         }

         snapshot->write_section<value_t>([this]( auto& section ){
            decltype(utils)::walk(db, [this, &section]( const auto &row ) {
               section.add_row(row, db);
            });
         });
      });

      add_contract_tables_to_snapshot(snapshot);

      authorization.add_to_snapshot(snapshot);
      resource_limits.add_to_snapshot(snapshot);
   }

   void read_from_snapshot( const snapshot_reader_ptr& snapshot ) {
      snapshot->read_section<chain_snapshot_header>([this]( auto &section ){
         chain_snapshot_header header;
         section.read_row(header, db);
         header.validate();
      });


      snapshot->read_section<block_state>([this]( auto &section ){
         block_header_state head_header_state;
         section.read_row(head_header_state, db);

         auto head_state = std::make_shared<block_state>(head_header_state);
         fork_db.set(head_state);
         fork_db.set_validity(head_state, true);
         fork_db.mark_in_current_chain(head_state, true);
         head = head_state;
         snapshot_head_block = head->block_num;
      });

      controller_index_set::walk_indices([this, &snapshot]( auto utils ){
         using value_t = typename decltype(utils)::index_t::value_type;

         // skip the table_id_object as its inlined with contract tables section
         if (std::is_same<value_t, table_id_object>::value) {
            return;
         }

         snapshot->read_section<value_t>([this]( auto& section ) {
            bool more = !section.empty();
            while(more) {
               decltype(utils)::create(db, [this, &section, &more]( auto &row ) {
                  more = section.read_row(row, db);
               });
            }
         });
      });

      read_contract_tables_from_snapshot(snapshot);

      authorization.read_from_snapshot(snapshot);
      resource_limits.read_from_snapshot(snapshot);

      db.set_revision( head->block_num );
   }

   sha256 calculate_integrity_hash() const {
      sha256::encoder enc;
      auto hash_writer = std::make_shared<integrity_hash_snapshot_writer>(enc);
      add_to_snapshot(hash_writer);
      hash_writer->finalize();

      return enc.result();
   }


   /**
    *  Sets fork database head to the genesis state.
    */
   void initialize_fork_db() {
      wlog( " Initializing new blockchain with genesis state                  " );
      producer_schedule_type initial_schedule;
      initialize_schedule(initial_schedule);

      block_header_state genheader;
      genheader.active_schedule       = initial_schedule;
      genheader.pending_schedule      = initial_schedule;
      genheader.pending_schedule_hash = fc::sha256::hash(initial_schedule);
      genheader.header.timestamp      = conf.genesis.initial_timestamp;
      genheader.header.action_mroot   = conf.genesis.compute_chain_id();
      genheader.id                    = genheader.header.id();
      genheader.block_num             = genheader.header.block_num();

      head = std::make_shared<block_state>( genheader );
      head->block = std::make_shared<signed_block>(genheader.header);
      fork_db.set( head );
      db.set_revision( head->block_num );

      initialize_database();
   }

   void create_native_account( account_name name, const authority& owner, const authority& active, bool is_privileged = false ) {
      if (db.find<account_object, by_name>(name) != nullptr) {
        // elog("create_native_account, This account already exists : ${name}", ("name", name));
        return;
      }
      db.create<account_object>([&](auto& a) {
         a.name = name;
         a.creation_date = conf.genesis.initial_timestamp;
         a.privileged = is_privileged;

         if( name == config::system_account_name ) {
            a.set_abi(eosio_contract_abi(abi_def()));
         }
      });
      db.create<account_sequence_object>([&](auto & a) {
        a.name = name;
      });

      const auto& owner_permission  = authorization.create_permission(name, config::owner_name, 0,
                                                                      owner, conf.genesis.initial_timestamp );
      const auto& active_permission = authorization.create_permission(name, config::active_name, owner_permission.id,
                                                                      active, conf.genesis.initial_timestamp );

      resource_limits.initialize_account(name);

      int64_t ram_delta = config::overhead_per_account_ram_bytes;
      ram_delta += 2*config::billable_size_v<permission_object>;
      ram_delta += owner_permission.auth.get_billable_size();
      ram_delta += active_permission.auth.get_billable_size();

      resource_limits.add_pending_ram_usage(name, ram_delta);
      resource_limits.verify_account_ram_usage(name);
   }

   // initialize_producer init bios bps in initial_producer_list
   void initialize_producer() {
      auto db = memory_db(self);
      for( const auto& producer : conf.genesis.initial_producer_list ) {
         // create accpimt for init bps
         const authority auth(producer.bpkey);
         create_native_account(producer.name, auth, auth, false);

         // store bp data in bp table
         db.insert(config::system_account_name, config::system_account_name, N(bps),
                   producer.name,
                   memory_db::bp_info{
                         producer.name,
                         producer.bpkey,
                         producer.commission_rate,
                         producer.url});
      }
   }

   // initialize_chain_emergency init chain emergency stat
   void initialize_chain_emergency() {
      memory_db(self).insert(
            config::system_account_name, config::system_account_name, N(chainstatus),
            config::system_account_name,
            memory_db::chain_status{N(chainstatus), false});
   }

   // initialize_account init account from genesis;
   // inactive account freeze(lock) asset by inactive_freeze_percent;
   void initialize_account() {
      std::set<account_name> active_acc_set;
      for (const auto &account : conf.active_initial_account_list) {
         active_acc_set.insert(account.name);
      }

      const auto acc_name_a = N(a);
      auto db = memory_db(self);
      for (const auto &account : conf.genesis.initial_account_list) {
         const auto &public_key = account.key;
         auto acc_name = account.name;
         if (acc_name == acc_name_a) {
            const auto pk_str = std::string(public_key);
            const auto name_r = pk_str.substr(pk_str.size() - 12, 12);
            acc_name = string_to_name(format_name(name_r).c_str());
         }

         // init asset
         eosio::chain::asset amount;
         if (active_acc_set.find(account.name) == active_acc_set.end()) {
            //issue eoslock token to this account
            uint64_t eoslock_amount = account.asset.get_amount() * conf.inactive_freeze_percent / 100;
            db.insert(
                    config::eoslock_account_name, config::eoslock_account_name, N(accounts), acc_name,
                     memory_db::eoslock_account{acc_name, eosio::chain::asset(eoslock_amount, symbol(4, "EOSLOCK"))});

            //inactive account freeze(lock) asset
            amount = account.asset - eosio::chain::asset(eoslock_amount);
         } else {
            //active account
            amount = account.asset;
         }

         // initialize_account_to_table
         db.insert(
                 config::system_account_name, config::system_account_name, N(accounts), acc_name,
                 memory_db::account_info{acc_name, amount});
         const authority auth(public_key);
         create_native_account(acc_name, auth, auth, false);
      }
   }

   // initialize_contract init sys contract
   void initialize_contract( const uint64_t& contract,
                             const bytes& code,
                             const bytes& abi,
                             const bool privileged = false ) {
      const auto& code_id = fc::sha256::hash(code.data(), (uint32_t) code.size());
      const int64_t code_size = code.size();
      const int64_t abi_size = abi.size();

      const auto& account = db.get<account_object, by_name>(contract);
      db.modify(account, [&]( auto& a ) {
         a.last_code_update = conf.genesis.initial_timestamp;
         a.privileged = privileged;

         a.code_version = code_id;
         a.code.resize(code_size);
         memcpy(a.code.data(), code.data(), code_size);

         a.abi.resize(abi_size);
         if( abi_size > 0 ) {
            memcpy(a.abi.data(), abi.data(), abi_size);
         }
      });

      const auto& account_sequence = db.get<account_sequence_object, by_name>(contract);
      db.modify(account_sequence, [&]( auto& aso ) {
         aso.code_sequence += 1;
         aso.abi_sequence += 1;
      });

      const auto& usage  = db.get<resource_limits::resource_usage_object, resource_limits::by_owner>( contract );
      db.modify( usage, [&]( auto& u ) {
          u.ram_usage += (code_size + abi_size) * config::setcode_ram_bytes_multiplier;
      });

      ilog("initialize_contract: name:${n}, code_size:${code}, abi_size:${abi}", ("n", account.name)("code",code_size)("abi",abi_size));
   }

   // initialize_eos_stats init stats for eos token
   void initialize_eos_stats() {
      const auto& sym = symbol(CORE_SYMBOL).to_symbol_code();
      memory_db(self).insert(config::token_account_name, sym, N(stat),
                             config::token_account_name,
                             memory_db::currency_stats{
                                   asset(10000000),
                                   asset(100000000000),
                                   config::token_account_name});
   }

   void initialize_database() {
      // Initialize block summary index
      for (int i = 0; i < 0x10000; i++)
         db.create<block_summary_object>([&](block_summary_object&) {});

      const auto& tapos_block_summary = db.get<block_summary_object>(1);
      db.modify( tapos_block_summary, [&]( auto& bs ) {
        bs.block_id = head->id;
      });

      conf.genesis.initial_configuration.validate();
      db.create<global_property_object>([&](auto& gpo ){
        gpo.configuration = conf.genesis.initial_configuration;
      });
      db.create<dynamic_global_property_object>([](auto&){});

      authorization.initialize_database();
      resource_limits.initialize_database();

      authority system_auth( conf.genesis.initial_key );
      create_native_account( config::system_account_name, system_auth, system_auth, true );
      create_native_account( config::token_account_name, system_auth, system_auth, false );
      create_native_account( config::eoslock_account_name, system_auth, system_auth, false );

      initialize_contract( config::system_account_name, conf.System_code, conf.System_abi, true );
      initialize_contract( config::token_account_name, conf.token_code, conf.token_abi );
      initialize_eos_stats();
      initialize_contract(config::eoslock_account_name, conf.lock_code, conf.lock_abi);

      initialize_account();
      initialize_producer();
      initialize_chain_emergency();

      // vote4ram func, as the early eosforce user's ram not limit
      // so at first we set freeram to -1 to unlimit user ram
      set_num_config_on_chain(db, config::res_typ::free_ram_per_account, -1);

      auto empty_authority = authority(1, {}, {});
      auto active_producers_authority = authority(1, {}, {});
      active_producers_authority.accounts.push_back({{config::system_account_name, config::active_name}, 1});

      create_native_account( config::null_account_name, empty_authority, empty_authority );
      create_native_account( config::producers_account_name, empty_authority, active_producers_authority );
      const auto& active_permission       = authorization.get_permission({config::producers_account_name, config::active_name});
      const auto& majority_permission     = authorization.create_permission( config::producers_account_name,
                                                                             config::majority_producers_permission_name,
                                                                             active_permission.id,
                                                                             active_producers_authority,
                                                                             conf.genesis.initial_timestamp );
      const auto& minority_permission     = authorization.create_permission( config::producers_account_name,
                                                                             config::minority_producers_permission_name,
                                                                             majority_permission.id,
                                                                             active_producers_authority,
                                                                             conf.genesis.initial_timestamp );
   }



   /**
    * @post regardless of the success of commit block there is no active pending block
    */
   void commit_block( bool add_to_fork_db ) {
      auto reset_pending_on_exit = fc::make_scoped_exit([this]{
         pending.reset();
      });

      try {
         if (add_to_fork_db) {
            pending->_pending_block_state->validated = true;
            auto new_bsp = fork_db.add(pending->_pending_block_state, true);
            emit(self.accepted_block_header, pending->_pending_block_state);
            head = fork_db.head();
            EOS_ASSERT(new_bsp == head, fork_database_exception, "committed block did not become the new head in fork database");
         }

         if( !replaying ) {
            reversible_blocks.create<reversible_block_object>( [&]( auto& ubo ) {
               ubo.blocknum = pending->_pending_block_state->block_num;
               ubo.set_block( pending->_pending_block_state->block );
            });
         }

         emit( self.accepted_block, pending->_pending_block_state );
      } catch (...) {
         // dont bother resetting pending, instead abort the block
         reset_pending_on_exit.cancel();
         abort_block();
         throw;
      }

      // push the state for pending.
      pending->push();
   }

   // The returned scoped_exit should not exceed the lifetime of the pending which existed when make_block_restore_point was called.
   fc::scoped_exit<std::function<void()>> make_block_restore_point() {
      auto orig_block_transactions_size = pending->_pending_block_state->block->transactions.size();
      auto orig_state_transactions_size = pending->_pending_block_state->trxs.size();
      auto orig_state_actions_size      = pending->_actions.size();

      std::function<void()> callback = [this,
                                        orig_block_transactions_size,
                                        orig_state_transactions_size,
                                        orig_state_actions_size]()
      {
         pending->_pending_block_state->block->transactions.resize(orig_block_transactions_size);
         pending->_pending_block_state->trxs.resize(orig_state_transactions_size);
         pending->_actions.resize(orig_state_actions_size);
      };

      return fc::make_scoped_exit( std::move(callback) );
   }

   transaction_trace_ptr apply_onerror( const generated_transaction& gtrx,
                                        fc::time_point deadline,
                                        fc::time_point start,
                                        uint32_t& cpu_time_to_bill_us, // only set on failure
                                        uint32_t billed_cpu_time_us,
                                        bool explicit_billed_cpu_time = false,
                                        bool enforce_whiteblacklist = true
                                      )
   {
      signed_transaction etrx;
      // Deliver onerror action containing the failed deferred transaction directly back to the sender.
      etrx.actions.emplace_back( vector<permission_level>{{gtrx.sender, config::active_name}},
                                 onerror( gtrx.sender_id, gtrx.packed_trx.data(), gtrx.packed_trx.size() ) );
      etrx.expiration = self.pending_block_time() + fc::microseconds(999'999); // Round up to avoid appearing expired
      etrx.set_reference_block( self.head_block_id() );

      transaction_context trx_context( self, etrx, etrx.id(), start );
      trx_context.deadline = deadline;
      trx_context.explicit_billed_cpu_time = explicit_billed_cpu_time;
      trx_context.billed_cpu_time_us = billed_cpu_time_us;
      trx_context.enforce_whiteblacklist = enforce_whiteblacklist;
      transaction_trace_ptr trace = trx_context.trace;
      try {
         trx_context.init_for_implicit_trx();
         trx_context.published = gtrx.published;
         trx_context.trace->action_traces.emplace_back();
         trx_context.dispatch_action( trx_context.trace->action_traces.back(), etrx.actions.back(), gtrx.sender );
         trx_context.finalize(); // Automatically rounds up network and CPU usage in trace and bills payers if successful

         auto restore = make_block_restore_point();
         trace->receipt = push_receipt( gtrx.trx_id, transaction_receipt::soft_fail,
                                        trx_context.billed_cpu_time_us, trace->net_usage );
         fc::move_append( pending->_actions, move(trx_context.executed) );

         trx_context.squash();
         restore.cancel();
         return trace;
      } catch( const fc::exception& e ) {
         cpu_time_to_bill_us = trx_context.update_billed_cpu_time( fc::time_point::now() );
         trace->except = e;
         trace->except_ptr = std::current_exception();
      }
      return trace;
   }

   void remove_scheduled_transaction( const generated_transaction_object& gto ) {
      resource_limits.add_pending_ram_usage(
         gto.payer,
         -(config::billable_size_v<generated_transaction_object> + gto.packed_trx.size())
      );
      // No need to verify_account_ram_usage since we are only reducing memory

      db.remove( gto );
   }

   bool failure_is_subjective( const fc::exception& e ) const {
      auto code = e.code();
      return    (code == subjective_block_production_exception::code_value)
             || (code == block_net_usage_exceeded::code_value)
             || (code == greylist_net_usage_exceeded::code_value)
             || (code == block_cpu_usage_exceeded::code_value)
             || (code == greylist_cpu_usage_exceeded::code_value)
             || (code == deadline_exception::code_value)
             || (code == leeway_deadline_exception::code_value)
             || (code == actor_whitelist_exception::code_value)
             || (code == actor_blacklist_exception::code_value)
             || (code == contract_whitelist_exception::code_value)
             || (code == contract_blacklist_exception::code_value)
             || (code == action_blacklist_exception::code_value)
             || (code == key_blacklist_exception::code_value);
   }

   bool scheduled_failure_is_subjective( const fc::exception& e ) const {
      auto code = e.code();
      return    (code == tx_cpu_usage_exceeded::code_value)
             || failure_is_subjective(e);
   }

   transaction_trace_ptr push_scheduled_transaction( const transaction_id_type& trxid, fc::time_point deadline, uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time = false ) {
      const auto& idx = db.get_index<generated_transaction_multi_index,by_trx_id>();
      auto itr = idx.find( trxid );
      EOS_ASSERT( itr != idx.end(), unknown_transaction_exception, "unknown transaction" );
      return push_scheduled_transaction( *itr, deadline, billed_cpu_time_us, explicit_billed_cpu_time );
   }

   transaction_trace_ptr push_scheduled_transaction( const generated_transaction_object& gto, fc::time_point deadline, uint32_t billed_cpu_time_us, bool explicit_billed_cpu_time = false )
   { try {
      maybe_session undo_session;
      if ( !self.skip_db_sessions() )
         undo_session = maybe_session(db);

      auto gtrx = generated_transaction(gto);

      // remove the generated transaction object after making a copy
      // this will ensure that anything which affects the GTO multi-index-container will not invalidate
      // data we need to successfully retire this transaction.
      //
      // IF the transaction FAILs in a subjective way, `undo_session` should expire without being squashed
      // resulting in the GTO being restored and available for a future block to retire.
      remove_scheduled_transaction(gto);

      fc::datastream<const char*> ds( gtrx.packed_trx.data(), gtrx.packed_trx.size() );

      EOS_ASSERT( gtrx.delay_until <= self.pending_block_time(), transaction_exception, "this transaction isn't ready",
                 ("gtrx.delay_until",gtrx.delay_until)("pbt",self.pending_block_time())          );

      signed_transaction dtrx;
      fc::raw::unpack(ds,static_cast<transaction&>(dtrx) );
      transaction_metadata_ptr trx = std::make_shared<transaction_metadata>( dtrx );
      trx->accepted = true;
      trx->scheduled = true;

      transaction_trace_ptr trace;
      if( gtrx.expiration < self.pending_block_time() ) {
         trace = std::make_shared<transaction_trace>();
         trace->id = gtrx.trx_id;
         trace->block_num = self.pending_block_state()->block_num;
         trace->block_time = self.pending_block_time();
         trace->producer_block_id = self.pending_producer_block_id();
         trace->scheduled = true;
         trace->receipt = push_receipt( gtrx.trx_id, transaction_receipt::expired, billed_cpu_time_us, 0 ); // expire the transaction
         emit( self.accepted_transaction, trx );
         emit( self.applied_transaction, trace );
         undo_session.squash();
         return trace;
      }

      // is_onfee_act on early version eosforce we use a trx contain onfee act before do trx
      // new version use a onfee act in the trx, when exec trx, a onfee action will do first
      const auto is_onfee_act = is_func_has_open(self, config::func_typ::onfee_action);

      auto reset_in_trx_requiring_checks = fc::make_scoped_exit([old_value=in_trx_requiring_checks,this](){
         in_trx_requiring_checks = old_value;
      });
      in_trx_requiring_checks = true;

      uint32_t cpu_time_to_bill_us = billed_cpu_time_us;

      transaction_context trx_context( self, dtrx, gtrx.trx_id );
      trx_context.leeway =  fc::microseconds(0); // avoid stealing cpu resource
      trx_context.deadline = deadline;
      trx_context.explicit_billed_cpu_time = explicit_billed_cpu_time;
      trx_context.billed_cpu_time_us = billed_cpu_time_us;
      trx_context.enforce_whiteblacklist = gtrx.sender.empty() ? true : !sender_avoids_whitelist_blacklist_enforcement( gtrx.sender );
      trace = trx_context.trace;
      try {
          //action check
          check_action(dtrx.actions);
          asset fee_ext = dtrx.fee;

         trx_context.init_for_deferred_trx( gtrx.published );
         if( !is_onfee_act ) {
            trx_context.make_limit_by_contract(fee_ext);
         }else{
            trx_context.make_fee_act(fee_ext);
         }

         if( trx_context.enforce_whiteblacklist && pending->_block_status == controller::block_status::incomplete ) {
            check_actor_list( trx_context.bill_to_accounts ); // Assumes bill_to_accounts is the set of actors authorizing the transaction
         }

         trx_context.exec();
         trx_context.finalize(); // Automatically rounds up network and CPU usage in trace and bills payers if successful

         auto restore = make_block_restore_point();

         trace->receipt = push_receipt( gtrx.trx_id,
                                        transaction_receipt::executed,
                                        trx_context.billed_cpu_time_us,
                                        trace->net_usage );

         fc::move_append( pending->_actions, move(trx_context.executed) );

         emit( self.accepted_transaction, trx );
         emit( self.applied_transaction, trace );

         trx_context.squash();
         undo_session.squash();

         restore.cancel();

         return trace;
      } catch( const fc::exception& e ) {
         cpu_time_to_bill_us = trx_context.update_billed_cpu_time( fc::time_point::now() );
         trace->except = e;
         trace->except_ptr = std::current_exception();
         trace->elapsed = fc::time_point::now() - trx_context.start;
      }
      trx_context.undo();

      // Only subjective OR soft OR hard failure logic below:

      if( gtrx.sender != account_name() && !failure_is_subjective(*trace->except)) {
         // Attempt error handling for the generated transaction.

         auto error_trace = apply_onerror( gtrx, deadline, trx_context.pseudo_start,
                                           cpu_time_to_bill_us, billed_cpu_time_us, explicit_billed_cpu_time,
                                           trx_context.enforce_whiteblacklist );
         error_trace->failed_dtrx_trace = trace;
         trace = error_trace;
         if( !trace->except_ptr ) {
            emit( self.accepted_transaction, trx );
            emit( self.applied_transaction, trace );
            undo_session.squash();
            return trace;
         }
         trace->elapsed = fc::time_point::now() - trx_context.start;
      }

      // Only subjective OR hard failure logic below:

      // subjectivity changes based on producing vs validating
      bool subjective  = false;
      if (explicit_billed_cpu_time) {
         subjective = failure_is_subjective(*trace->except);
      } else {
         subjective = scheduled_failure_is_subjective(*trace->except);
      }

      if ( !subjective ) {
         // hard failure logic

         if( !explicit_billed_cpu_time ) {
            auto& rl = self.get_mutable_resource_limits_manager();
            rl.update_account_usage( trx_context.bill_to_accounts, block_timestamp_type(self.pending_block_time()).slot );
            int64_t account_cpu_limit = 0;
            std::tie( std::ignore, account_cpu_limit, std::ignore, std::ignore ) = trx_context.max_bandwidth_billed_accounts_can_pay( true );

            cpu_time_to_bill_us = static_cast<uint32_t>( std::min( std::min( static_cast<int64_t>(cpu_time_to_bill_us),
                                                                             account_cpu_limit                          ),
                                                                   trx_context.initial_objective_duration_limit.count()    ) );
         }

         resource_limits.add_transaction_usage( trx_context.bill_to_accounts, cpu_time_to_bill_us, 0,
                                                block_timestamp_type(self.pending_block_time()).slot ); // Should never fail

         trace->receipt = push_receipt(gtrx.trx_id, transaction_receipt::hard_fail, cpu_time_to_bill_us, 0);

         emit( self.accepted_transaction, trx );
         emit( self.applied_transaction, trace );

         undo_session.squash();
      } else {
         emit( self.accepted_transaction, trx );
         emit( self.applied_transaction, trace );
      }

      return trace;
   } FC_CAPTURE_AND_RETHROW() } /// push_scheduled_transaction


   /**
    *  Adds the transaction receipt to the pending block and returns it.
    */
   template<typename T>
   const transaction_receipt& push_receipt( const T& trx, transaction_receipt_header::status_enum status,
                                            uint64_t cpu_usage_us, uint64_t net_usage ) {
      uint64_t net_usage_words = net_usage / 8;
      // EOS_ASSERT( net_usage_words*8 == net_usage, transaction_exception, "net_usage is not divisible by 8" );
      pending->_pending_block_state->block->transactions.emplace_back( trx );
      transaction_receipt& r = pending->_pending_block_state->block->transactions.back();
      r.cpu_usage_us         = cpu_usage_us;
      r.net_usage_words      = net_usage_words;
      r.status               = status;
      return r;
   }

   bool check_chainstatus() const {
      const auto *cstatus_tid = db.find<table_id_object, by_code_scope_table>(
            boost::make_tuple(config::system_account_name, config::system_account_name, N(chainstatus)));

      EOS_ASSERT(cstatus_tid != nullptr, fork_database_exception, "get chainstatus fatal");

      const auto& idx = db.get_index<key_value_index, by_scope_primary>();
      const auto it = idx.lower_bound(boost::make_tuple(cstatus_tid->id, N(chainstatus)));

      EOS_ASSERT(( it != idx.end()), fork_database_exception, "get chainstatus fatal by no stat");

      auto cstatus = fc::raw::unpack<memory_db::chain_status>(
            it->value.data(),
            it->value.size());
      return cstatus.emergency;
   }

   void check_action( const vector<action>& actions ) const {
      const auto chain_status = check_chainstatus();
      const auto trx_size_limit = get_num_config_on_chain(db,
            config::res_typ::trx_size_limit,
            config::default_trx_size);
      for( const auto& _a : actions ) {
         EOS_ASSERT(_a.data.size() < trx_size_limit,
                    invalid_action_args_exception,
                    "transaction size must less then trx_size_limit on chain");
         EOS_ASSERT(( !chain_status
                      || _a.name == N(setemergency)
                      || _a.name == N(onblock)
                      || _a.name == N(onfee)
                      || _a.name == N(voteagefee)),
                    invalid_action_args_exception,
                    "chain is in emergency now !");
      }
   }

   /**
    *  This is the entry point for new transactions to the block state. It will check authorization and
    *  determine whether to execute it now or to delay it. Lastly it inserts a transaction receipt into
    *  the pending block.
    */
   transaction_trace_ptr push_transaction( const transaction_metadata_ptr& trx,
                                           fc::time_point deadline,
                                           uint32_t billed_cpu_time_us,
                                           bool explicit_billed_cpu_time = false )
   {
      EOS_ASSERT(deadline != fc::time_point(), transaction_exception, "deadline cannot be uninitialized");
      //EOS_ASSERT(trx->trx.context_free_actions.size()==0, transaction_exception, "context free actions size should be zero!");
      check_action(trx->packed_trx->get_transaction().actions);

      transaction_trace_ptr trace;
      try {
         auto start = fc::time_point::now();
         const bool check_auth = !self.skip_auth_check() && !trx->implicit;
         // call recover keys so that trx->sig_cpu_usage is set correctly
         const fc::microseconds sig_cpu_usage = check_auth ? std::get<0>( trx->recover_keys( chain_id ) ) : fc::microseconds();
         const flat_set<public_key_type>& recovered_keys = check_auth ? std::get<1>( trx->recover_keys( chain_id ) ) : flat_set<public_key_type>();
         if( !explicit_billed_cpu_time ) {
            fc::microseconds already_consumed_time( EOS_PERCENT(sig_cpu_usage.count(), conf.sig_cpu_bill_pct) );

            if( start.time_since_epoch() <  already_consumed_time ) {
               start = fc::time_point();
            } else {
               start -= already_consumed_time;
            }
         }

         const signed_transaction& trn = trx->packed_trx->get_signed_transaction();
         transaction_context trx_context(self, trn, trx->id, start);
         if ((bool)subjective_cpu_leeway && pending->_block_status == controller::block_status::incomplete) {
            trx_context.leeway = *subjective_cpu_leeway;
         }
         trx_context.deadline = deadline;
         trx_context.explicit_billed_cpu_time = explicit_billed_cpu_time;
         trx_context.billed_cpu_time_us = billed_cpu_time_us;
         trace = trx_context.trace;
         try {
            if( trx->implicit ) {
               trx_context.init_for_implicit_trx();
               trx_context.enforce_whiteblacklist = false;
            } else {
               bool skip_recording = replay_head_time && (time_point(trn.expiration) <= *replay_head_time);
               trx_context.init_for_input_trx( trx->packed_trx->get_unprunable_size(),
                                               trx->packed_trx->get_prunable_size(),
                                               skip_recording);
            }

            trx_context.delay = fc::seconds(trn.delay_sec);
            // is_onfee_act on early version eosforce we use a trx contain onfee act before do trx
            // new version use a onfee act in the trx, when exec trx, a onfee action will do first

            const auto is_onfee_act = is_func_has_open(self, config::func_typ::onfee_action);
            const auto is_fee_limit = is_onfee_act && is_func_has_open(self, config::func_typ::fee_limit);

            asset fee_ext(0); // fee ext to get more res
            if( !trx->implicit ) {
               authorization.check_authorization(
                       trn.actions,
                       recovered_keys,
                       {},
                       trx_context.delay,
                       [&trx_context](){ trx_context.checktime(); },
                       false
               );

               if( !is_fee_limit ) {
                  const auto fee_required = txfee.get_required_fee(self, trn);
                  EOS_ASSERT(trn.fee >= fee_required, transaction_exception, "set tx fee failed: no enough fee in trx");
                  fee_ext = trn.fee - fee_required;
               }
               EOS_ASSERT(txfee.check_transaction(trn) == true, transaction_exception, "transaction include actor more than one");


               // keep
               if( !is_onfee_act ) {
                  try {
                     auto onftrx = std::make_shared<transaction_metadata>(
                           get_on_fee_transaction(trn.fee, trn.actions[0].authorization[0].actor));
                     onftrx->implicit = true;
                     auto onftrace = push_transaction(onftrx, fc::time_point::maximum(),
                                                      config::default_min_transaction_cpu_usage, true);
                     if( onftrace->except ) throw *onftrace->except;
                  } catch( const fc::exception& e ) {
                     EOS_ASSERT(false, transaction_exception, "on fee transaction failed, exception: ${e}", ( "e", e ));
                  } catch( ... ) {
                     EOS_ASSERT(false, transaction_exception,
                                "on fee transaction failed, but shouldn't enough asset to pay for transaction fee");
                  }
               } else {
                  asset fee_limit{ 0 };
                  get_from_extensions(trn.transaction_extensions, transaction::fee_limit, fee_limit);
                  trx_context.make_fee_act(fee_limit);
               }
            }

            try {
               if(explicit_billed_cpu_time && billed_cpu_time_us == 0){
                  EOS_ASSERT(false, transaction_exception, "error trx",
                      ("block", head->block_num)("trx", trn.id())("actios", trn.actions));
               }

               if( !is_onfee_act ) {
                  trx_context.make_limit_by_contract(fee_ext);
               }
               trx_context.exec();
               trx_context.finalize(); // Automatically rounds up network and CPU usage in trace and bills payers if successful
             } catch (const fc::exception &e) {
               // keep
               if( !is_onfee_act ) {
                  trace->except = e;
                  trace->except_ptr = std::current_exception();
               } else {
                  throw;
               }
             }

            auto restore = make_block_restore_point();

            if (!trx->implicit) {
               transaction_receipt::status_enum s = (trx_context.delay == fc::seconds(0))
                                                    ? transaction_receipt::executed
                                                    : transaction_receipt::delayed;
               trace->receipt = push_receipt(*trx->packed_trx, s, trx_context.billed_cpu_time_us, trace->net_usage);
               pending->_pending_block_state->trxs.emplace_back(trx);
            } else {
               transaction_receipt_header r;
               r.status = transaction_receipt::executed;
               r.cpu_usage_us = trx_context.billed_cpu_time_us;
               r.net_usage_words = trace->net_usage / 8;
               trace->receipt = r;
            }

            fc::move_append(pending->_actions, move(trx_context.executed));

            // call the accept signal but only once for this transaction
            if (!trx->accepted) {
               trx->accepted = true;
               emit( self.accepted_transaction, trx);
            }

            emit(self.applied_transaction, trace);


            if ( read_mode != db_read_mode::SPECULATIVE && pending->_block_status == controller::block_status::incomplete ) {
               //this may happen automatically in destructor, but I prefere make it more explicit
               trx_context.undo();
            } else {
               restore.cancel();
               trx_context.squash();
            }

            if (!trx->implicit) {
               unapplied_transactions.erase( trx->signed_id );
            }
            return trace;
         } catch (const fc::exception& e) {
            trace->except = e;
            trace->except_ptr = std::current_exception();
         }

         if (!failure_is_subjective(*trace->except)) {
            unapplied_transactions.erase( trx->signed_id );
         }

         emit( self.accepted_transaction, trx );
         emit( self.applied_transaction, trace );

         return trace;
      } FC_CAPTURE_AND_RETHROW((trace))
   } /// push_transaction


   // check_func_open
   void check_func_open() {
      // when on the specific block : load new System contract
      if( is_func_open_in_curr_block( self, config::func_typ::use_system01, 3385100 ) ) {
         initialize_contract(config::system_account_name, conf.System01_code, conf.System01_abi, true);
      }

      // when on the specific block : load eosio.msig contract
      if( is_func_open_in_curr_block( self, config::func_typ::use_msig, 4356456 ) ) {
         initialize_contract(config::msig_account_name, conf.msig_code, conf.msig_abi, true);
      }

      // when on the specific block : update auth eosio@active to eosio.prods@active
      if( is_func_open_in_curr_block( self, config::func_typ::use_eosio_prods) ) {
         ilog("update auth eosio@active to eosio.prods@active");
         update_eosio_authority();
      }

      // vote4ram func, as the early eosforce user's ram not limit
      // so at first we set freeram to -1 to unlimit user ram
      // when vote4ram open, change to 8kb per user
      if( is_func_open_in_curr_block(self, config::func_typ::vote_for_ram) ) {
         set_num_config_on_chain(db, config::res_typ::free_ram_per_account, 8 * 1024);
      }

       // when on the specific block : create eosio account in table accounts of eosio system contract
      if (is_func_has_open(self, config::func_typ::create_eosio_account, 5814500)) {
         auto db = memory_db(self);
         memory_db::account_info acc;
         if (!db.get(config::system_account_name, config::system_account_name, N(accounts),
                     config::system_account_name, acc)) {
            db.insert(config::system_account_name, config::system_account_name, N(accounts),
                      config::system_account_name,
                      memory_db::account_info{config::system_account_name, eosio::chain::asset(0)});
         }
      }

      // when on the specific block : create eosio account in table accounts of eosio system contract
      if (is_func_open_in_curr_block(self, config::func_typ::create_prods_account)) {
         auto db = memory_db(self);
         memory_db::account_info acc;
         if (!db.get(config::system_account_name, 
                     config::system_account_name, N(accounts), 
                     config::producers_account_name, acc)) {
            db.insert(config::system_account_name, config::system_account_name, N(accounts),
                      config::producers_account_name, memory_db::account_info{
                         config::producers_account_name, 
                         eosio::chain::asset(0)});
         }
      }
   }


   void start_block( block_timestamp_type when, uint16_t confirm_block_count, controller::block_status s,
                     const optional<block_id_type>& producer_block_id )
   {
      EOS_ASSERT( !pending, block_validate_exception, "pending block already exists" );

      auto guard_pending = fc::make_scoped_exit([this](){
         pending.reset();
      });

      if (!self.skip_db_sessions(s)) {
         EOS_ASSERT( db.revision() == head->block_num, database_exception, "db revision is not on par with head block",
                     ("db.revision()", db.revision())("controller_head_block", head->block_num)("fork_db_head_block", fork_db.head()->block_num) );

         pending.emplace(maybe_session(db));
      } else {
         pending.emplace(maybe_session());
      }

      pending->_block_status = s;
      pending->_producer_block_id = producer_block_id;
      pending->_pending_block_state = std::make_shared<block_state>( *head, when ); // promotes pending schedule (if any) to active
      pending->_pending_block_state->in_current_chain = true;

      pending->_pending_block_state->set_confirmed(confirm_block_count);

      auto was_pending_promoted = pending->_pending_block_state->maybe_promote_pending();

      //modify state in speculative block only if we are speculative reads mode (other wise we need clean state for head or irreversible reads)
      if ( read_mode == db_read_mode::SPECULATIVE || pending->_block_status != controller::block_status::incomplete ) {

         const auto& gpo = db.get<global_property_object>();
         if( gpo.proposed_schedule_block_num.valid() && // if there is a proposed schedule that was proposed in a block ...
             ( *gpo.proposed_schedule_block_num <= pending->_pending_block_state->dpos_irreversible_blocknum ) && // ... that has now become irreversible ...
             pending->_pending_block_state->pending_schedule.producers.size() == 0 && // ... and there is room for a new pending schedule ...
             !was_pending_promoted // ... and not just because it was promoted to active at the start of this block, then:
         )
            {
               // Promote proposed schedule to pending schedule.
               if( !replaying ) {
                  ilog( "promoting proposed schedule (set in block ${proposed_num}) to pending; current block: ${n} lib: ${lib} schedule: ${schedule} ",
                        ("proposed_num", *gpo.proposed_schedule_block_num)("n", pending->_pending_block_state->block_num)
                        ("lib", pending->_pending_block_state->dpos_irreversible_blocknum)
                        ("schedule", static_cast<producer_schedule_type>(gpo.proposed_schedule) ) );
               }
               pending->_pending_block_state->set_new_producers( gpo.proposed_schedule );
               db.modify( gpo, [&]( auto& gp ) {
                     gp.proposed_schedule_block_num = optional<block_num_type>();
                     gp.proposed_schedule.clear();
                  });
            }

         check_func_open();

         try {
            auto onbtrx = std::make_shared<transaction_metadata>( get_on_block_transaction() );
            onbtrx->implicit = true;
            auto reset_in_trx_requiring_checks = fc::make_scoped_exit([old_value=in_trx_requiring_checks,this](){
                  in_trx_requiring_checks = old_value;
               });
            in_trx_requiring_checks = true;
            push_transaction( onbtrx, fc::time_point::maximum(), self.get_global_properties().configuration.min_transaction_cpu_usage, true );
         } catch( const boost::interprocess::bad_alloc& e  ) {
            elog( "on block transaction failed due to a bad allocation" );
            throw;
         } catch( const fc::exception& e ) {
            wlog( "on block transaction failed, but shouldn't impact block generation, system contract needs update" );
            edump((e.to_detail_string()));
         } catch( ... ) {
         }

         clear_expired_input_transactions();
         update_producers_authority();
      }

      guard_pending.cancel();
   } // start_block



   void sign_block( const std::function<signature_type( const digest_type& )>& signer_callback  ) {
      auto p = pending->_pending_block_state;

      p->sign( signer_callback );

      static_cast<signed_block_header&>(*p->block) = p->header;
   } /// sign_block

   void apply_block( const signed_block_ptr& b, controller::block_status s ) { try {
      try {
         EOS_ASSERT( b->block_extensions.size() == 0, block_validate_exception, "no supported extensions" );
         auto producer_block_id = b->id();
         start_block( b->timestamp, b->confirmed, s , producer_block_id);

         std::vector<transaction_metadata_ptr> packed_transactions;
         packed_transactions.reserve( b->transactions.size() );
         for( const auto& receipt : b->transactions ) {
            if( receipt.trx.contains<packed_transaction>()) {
               auto& pt = receipt.trx.get<packed_transaction>();
               auto mtrx = std::make_shared<transaction_metadata>( std::make_shared<packed_transaction>( pt ) );
               if( !self.skip_auth_check() ) {
                  transaction_metadata::start_recover_keys( mtrx, thread_pool, chain_id, microseconds::maximum() );
               }
               packed_transactions.emplace_back( std::move( mtrx ) );
            }
         }

         transaction_trace_ptr trace;

         size_t packed_idx = 0;
         for( const auto& receipt : b->transactions ) {
            auto num_pending_receipts = pending->_pending_block_state->block->transactions.size();
            if( receipt.trx.contains<packed_transaction>() ) {
               trace = push_transaction( packed_transactions.at(packed_idx++), fc::time_point::maximum(), receipt.cpu_usage_us, true );
            } else if( receipt.trx.contains<transaction_id_type>() ) {
               trace = push_scheduled_transaction( receipt.trx.get<transaction_id_type>(), fc::time_point::maximum(), receipt.cpu_usage_us, true );
            } else {
               EOS_ASSERT( false, block_validate_exception, "encountered unexpected receipt type" );
            }

            bool transaction_failed =  trace && trace->except;
            bool transaction_can_fail = receipt.status == transaction_receipt_header::hard_fail && receipt.trx.contains<transaction_id_type>();
            if( transaction_failed && !transaction_can_fail) {
               edump((*trace));
               // the eosio 's block not contain the block which has error,
               // so general a block 's push_transaction func called by apply_block in other pb should no exception.
               // but in eosforce block will include error, this will make chain error,
               // so eosforce should no throw
               // throw *trace->except;
            }

            EOS_ASSERT( pending->_pending_block_state->block->transactions.size() > 0,
                        block_validate_exception, "expected a receipt",
                        ("block", *b)("expected_receipt", receipt)
                      );
            EOS_ASSERT( pending->_pending_block_state->block->transactions.size() == num_pending_receipts + 1,
                        block_validate_exception, "expected receipt was not added",
                        ("block", *b)("expected_receipt", receipt)
                      );
            const transaction_receipt_header& r = pending->_pending_block_state->block->transactions.back();
            EOS_ASSERT( r == static_cast<const transaction_receipt_header&>(receipt),
                        block_validate_exception, "receipt does not match",
                        ("producer_receipt", receipt)("validator_receipt", pending->_pending_block_state->block->transactions.back()) );
         }

         finalize_block();

         // this implicitly asserts that all header fields (less the signature) are identical
         EOS_ASSERT(producer_block_id == pending->_pending_block_state->header.id(),
                   block_validate_exception, "Block ID does not match",
                   ("producer_block_id",producer_block_id)("validator_block_id",pending->_pending_block_state->header.id()));

         // We need to fill out the pending block state's block because that gets serialized in the reversible block log
         // in the future we can optimize this by serializing the original and not the copy

         // we can always trust this signature because,
         //   - prior to apply_block, we call fork_db.add which does a signature check IFF the block is untrusted
         //   - OTHERWISE the block is trusted and therefore we trust that the signature is valid
         // Also, as ::sign_block does not lazily calculate the digest of the block, we can just short-circuit to save cycles
         pending->_pending_block_state->header.producer_signature = b->producer_signature;
         static_cast<signed_block_header&>(*pending->_pending_block_state->block) =  pending->_pending_block_state->header;

         commit_block(false);
         return;
      } catch ( const fc::exception& e ) {
         edump((e.to_detail_string()));
         abort_block();
         throw;
      }
   } FC_CAPTURE_AND_RETHROW() } /// apply_block

   std::future<block_state_ptr> create_block_state_future( const signed_block_ptr& b ) {
      EOS_ASSERT( b, block_validate_exception, "null block" );

      auto id = b->id();

      // no reason for a block_state if fork_db already knows about block
      auto existing = fork_db.get_block( id );
      EOS_ASSERT( !existing, fork_database_exception, "we already know about this block: ${id}", ("id", id) );

      auto prev = fork_db.get_block( b->previous );
      EOS_ASSERT( prev, unlinkable_block_exception, "unlinkable block ${id}", ("id", id)("previous", b->previous) );

      return async_thread_pool( thread_pool, [b, prev]() {
         const bool skip_validate_signee = false;
         return std::make_shared<block_state>( *prev, move( b ), skip_validate_signee );
      } );
   }

   void push_block( std::future<block_state_ptr>& block_state_future ) {
      controller::block_status s = controller::block_status::complete;
      EOS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      auto reset_prod_light_validation = fc::make_scoped_exit([old_value=trusted_producer_light_validation, this]() {
         trusted_producer_light_validation = old_value;
      });
      try {
         block_state_ptr new_header_state = block_state_future.get();
         auto& b = new_header_state->block;
         emit( self.pre_accepted_block, b );

         fork_db.add( new_header_state, false );

         if (conf.trusted_producers.count(b->producer)) {
            trusted_producer_light_validation = true;
         };
         emit( self.accepted_block_header, new_header_state );

         if ( read_mode != db_read_mode::IRREVERSIBLE ) {
            maybe_switch_forks( s );
         }

      } FC_LOG_AND_RETHROW( )
   }

   void replay_push_block( const signed_block_ptr& b, controller::block_status s ) {
      self.validate_db_available_size();
      self.validate_reversible_available_size();

      EOS_ASSERT(!pending, block_validate_exception, "it is not valid to push a block when there is a pending block");

      try {
         EOS_ASSERT( b, block_validate_exception, "trying to push empty block" );
         EOS_ASSERT( (s == controller::block_status::irreversible || s == controller::block_status::validated),
                     block_validate_exception, "invalid block status for replay" );
         emit( self.pre_accepted_block, b );
         const bool skip_validate_signee = !conf.force_all_checks;
         auto new_header_state = fork_db.add( b, skip_validate_signee );

         emit( self.accepted_block_header, new_header_state );

         if ( read_mode != db_read_mode::IRREVERSIBLE ) {
            maybe_switch_forks( s );
         }

         // on replay irreversible is not emitted by fork database, so emit it explicitly here
         if( s == controller::block_status::irreversible )
            emit( self.irreversible_block, new_header_state );

      } FC_LOG_AND_RETHROW( )
   }

   void maybe_switch_forks( controller::block_status s ) {
      auto new_head = fork_db.head();

      if( new_head->header.previous == head->id ) {
         try {
            apply_block( new_head->block, s );
            fork_db.mark_in_current_chain( new_head, true );
            fork_db.set_validity( new_head, true );
            head = new_head;
         } catch ( const fc::exception& e ) {
            fork_db.set_validity( new_head, false ); // Removes new_head from fork_db index, so no need to mark it as not in the current chain.
            throw;
         }
      } else if( new_head->id != head->id ) {
         ilog("switching forks from ${current_head_id} (block number ${current_head_num}) to ${new_head_id} (block number ${new_head_num})",
              ("current_head_id", head->id)("current_head_num", head->block_num)("new_head_id", new_head->id)("new_head_num", new_head->block_num) );
         auto branches = fork_db.fetch_branch_from( new_head->id, head->id );

         for( auto itr = branches.second.begin(); itr != branches.second.end(); ++itr ) {
            fork_db.mark_in_current_chain( *itr, false );
            pop_block();
         }
         EOS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                     "loss of sync between fork_db and chainbase during fork switch" ); // _should_ never fail

         for( auto ritr = branches.first.rbegin(); ritr != branches.first.rend(); ++ritr ) {
            optional<fc::exception> except;
            try {
               apply_block( (*ritr)->block, (*ritr)->validated ? controller::block_status::validated : controller::block_status::complete );
               head = *ritr;
               fork_db.mark_in_current_chain( *ritr, true );
               (*ritr)->validated = true;
            }
            catch (const fc::exception& e) { except = e; }
            if (except) {
               elog("exception thrown while switching forks ${e}", ("e", except->to_detail_string()));

               // ritr currently points to the block that threw
               // if we mark it invalid it will automatically remove all forks built off it.
               fork_db.set_validity( *ritr, false );

               // pop all blocks from the bad fork
               // ritr base is a forward itr to the last block successfully applied
               auto applied_itr = ritr.base();
               for( auto itr = applied_itr; itr != branches.first.end(); ++itr ) {
                  fork_db.mark_in_current_chain( *itr, false );
                  pop_block();
               }
               EOS_ASSERT( self.head_block_id() == branches.second.back()->header.previous, fork_database_exception,
                           "loss of sync between fork_db and chainbase during fork switch reversal" ); // _should_ never fail

               // re-apply good blocks
               for( auto ritr = branches.second.rbegin(); ritr != branches.second.rend(); ++ritr ) {
                  apply_block( (*ritr)->block, controller::block_status::validated /* we previously validated these blocks*/ );
                  head = *ritr;
                  fork_db.mark_in_current_chain( *ritr, true );
               }
               throw *except;
            } // end if exception
         } /// end for each block in branch
         ilog("successfully switched fork to new head ${new_head_id}", ("new_head_id", new_head->id) );
      }
   } /// push_block

   void abort_block() {
      if( pending ) {
         if ( read_mode == db_read_mode::SPECULATIVE ) {
            for( const auto& t : pending->_pending_block_state->trxs )
               unapplied_transactions[t->signed_id] = t;
         }
         pending.reset();
      }
   }


   bool should_enforce_runtime_limits()const {
      return false;
   }

   void set_action_merkle() {
      vector<digest_type> action_digests;
      action_digests.reserve( pending->_actions.size() );
      for( const auto& a : pending->_actions )
         action_digests.emplace_back( a.digest() );

      pending->_pending_block_state->header.action_mroot = merkle( move(action_digests) );
   }

   void set_trx_merkle() {
      vector<digest_type> trx_digests;
      const auto& trxs = pending->_pending_block_state->block->transactions;
      trx_digests.reserve( trxs.size() );
      for( const auto& a : trxs )
         trx_digests.emplace_back( a.digest() );

      pending->_pending_block_state->header.transaction_mroot = merkle( move(trx_digests) );
   }


   void finalize_block()
   {
      EOS_ASSERT(pending, block_validate_exception, "it is not valid to finalize when there is no pending block");
      try {


      /*
      ilog( "finalize block ${n} (${id}) at ${t} by ${p} (${signing_key}); schedule_version: ${v} lib: ${lib} #dtrxs: ${ndtrxs} ${np}",
            ("n",pending->_pending_block_state->block_num)
            ("id",pending->_pending_block_state->header.id())
            ("t",pending->_pending_block_state->header.timestamp)
            ("p",pending->_pending_block_state->header.producer)
            ("signing_key", pending->_pending_block_state->block_signing_key)
            ("v",pending->_pending_block_state->header.schedule_version)
            ("lib",pending->_pending_block_state->dpos_irreversible_blocknum)
            ("ndtrxs",db.get_index<generated_transaction_multi_index,by_trx_id>().size())
            ("np",pending->_pending_block_state->header.new_producers)
            );
      */

      // Update resource limits:
      resource_limits.process_account_limit_updates();
      const auto& chain_config = self.get_global_properties().configuration;
      uint32_t max_virtual_mult = 1000;
      uint64_t CPU_TARGET = EOS_PERCENT(chain_config.max_block_cpu_usage, chain_config.target_block_cpu_usage_pct);
      resource_limits.set_block_parameters(
         { CPU_TARGET, chain_config.max_block_cpu_usage, config::block_cpu_usage_average_window_ms / config::block_interval_ms, max_virtual_mult, {99, 100}, {1000, 999}},
         {EOS_PERCENT(chain_config.max_block_net_usage, chain_config.target_block_net_usage_pct), chain_config.max_block_net_usage, config::block_size_average_window_ms / config::block_interval_ms, max_virtual_mult, {99, 100}, {1000, 999}}
      );
      resource_limits.process_block_usage(pending->_pending_block_state->block_num);

      set_action_merkle();
      set_trx_merkle();

      auto p = pending->_pending_block_state;
      p->id = p->header.id();

      create_block_summary(p->id);

   } FC_CAPTURE_AND_RETHROW() }

    void update_eosio_authority() {
        auto update_permission = [&]( auto& permission, auto threshold ) {
            auto auth = authority( threshold, {}, {});
            auth.accounts.push_back({{config::producers_account_name, config::active_name}, 1});

            if( static_cast<authority>(permission.auth) != auth ) {
                db.modify(permission, [&]( auto& po ) {
                    po.auth = auth;
                });
            }
        };

        update_permission( authorization.get_permission({config::system_account_name, config::active_name}), 1);
    }

   void update_producers_authority() {
      const auto& producers = pending->_pending_block_state->active_schedule.producers;

      auto update_permission = [&]( auto& permission, auto threshold ) {
         auto auth = authority( threshold, {}, {});
         for( auto& p : producers ) {
            auth.accounts.push_back({{p.producer_name, config::active_name}, 1});
         }

         if( static_cast<authority>(permission.auth) != auth ) { // TODO: use a more efficient way to check that authority has not changed
            db.modify(permission, [&]( auto& po ) {
               po.auth = auth;
            });
         }
      };

      uint32_t num_producers = producers.size();
      auto calculate_threshold = [=]( uint32_t numerator, uint32_t denominator ) {
         return ( (num_producers * numerator) / denominator ) + 1;
      };

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::active_name}),
                         calculate_threshold( 2, 3 ) /* more than two-thirds */                      );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::majority_producers_permission_name}),
                         calculate_threshold( 1, 2 ) /* more than one-half */                        );

      update_permission( authorization.get_permission({config::producers_account_name,
                                                       config::minority_producers_permission_name}),
                         calculate_threshold( 1, 3 ) /* more than one-third */                       );

      //TODO: Add tests
   }

   void create_block_summary(const block_id_type& id) {
      auto block_num = block_header::num_from_id(id);
      auto sid = block_num & 0xffff;
      db.modify( db.get<block_summary_object,by_id>(sid), [&](block_summary_object& bso ) {
          bso.block_id = id;
      });
   }


   void clear_expired_input_transactions() {
      //Look for expired transactions in the deduplication list, and remove them.
      auto& transaction_idx = db.get_mutable_index<transaction_multi_index>();
      const auto& dedupe_index = transaction_idx.indices().get<by_expiration>();
      auto now = self.pending_block_time();
      while( (!dedupe_index.empty()) && ( now > fc::time_point(dedupe_index.begin()->expiration) ) ) {
         transaction_idx.remove(*dedupe_index.begin());
      }
   }

   bool sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
      if( conf.sender_bypass_whiteblacklist.size() > 0 &&
          ( conf.sender_bypass_whiteblacklist.find( sender ) != conf.sender_bypass_whiteblacklist.end() ) )
      {
         return true;
      }

      return false;
   }

   void check_actor_list( const flat_set<account_name>& actors )const {
      if( actors.size() == 0 ) return;

      if( conf.actor_whitelist.size() > 0 ) {
         // throw if actors is not a subset of whitelist
         const auto& whitelist = conf.actor_whitelist;
         bool is_subset = true;

         // quick extents check, then brute force the check actors
         if (*actors.cbegin() >= *whitelist.cbegin() && *actors.crbegin() <= *whitelist.crbegin() ) {
            auto lower_bound = whitelist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, whitelist.cend(), actor);

               // if the actor is not found, this is not a subset
               if (lower_bound == whitelist.cend() || *lower_bound != actor ) {
                  is_subset = false;
                  break;
               }

               // if the actor was found, we are guaranteed that other actors are either not present in the whitelist
               // or will be present in the range defined as [next actor,end)
               lower_bound = std::next(lower_bound);
            }
         } else {
            is_subset = false;
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_missing_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& whitelist) -> vector<account_name> {
            vector<account_name> excluded;
            excluded.reserve( actors.size() );
            set_difference( actors.begin(), actors.end(),
                            whitelist.begin(), whitelist.end(),
                            std::back_inserter(excluded) );
            return excluded;
         };

         EOS_ASSERT( is_subset,  actor_whitelist_exception,
                     "authorizing actor(s) in transaction are not on the actor whitelist: ${actors}",
                     ("actors", generate_missing_actors(actors, whitelist))
                   );
      } else if( conf.actor_blacklist.size() > 0 ) {
         // throw if actors intersects blacklist
         const auto& blacklist = conf.actor_blacklist;
         bool intersects = false;

         // quick extents check then brute force check actors
         if( *actors.cbegin() <= *blacklist.crbegin() && *actors.crbegin() >= *blacklist.cbegin() ) {
            auto lower_bound = blacklist.cbegin();
            for (const auto& actor: actors) {
               lower_bound = std::lower_bound(lower_bound, blacklist.cend(), actor);

               // if the lower bound in the blacklist is at the end, all other actors are guaranteed to
               // not exist in the blacklist
               if (lower_bound == blacklist.cend()) {
                  break;
               }

               // if the lower bound of an actor IS the actor, then we have an intersection
               if (*lower_bound == actor) {
                  intersects = true;
                  break;
               }
            }
         }

         // helper lambda to lazily calculate the actors for error messaging
         static auto generate_blacklisted_actors = [](const flat_set<account_name>& actors, const flat_set<account_name>& blacklist) -> vector<account_name> {
            vector<account_name> blacklisted;
            blacklisted.reserve( actors.size() );
            set_intersection( actors.begin(), actors.end(),
                              blacklist.begin(), blacklist.end(),
                              std::back_inserter(blacklisted)
                            );
            return blacklisted;
         };

         EOS_ASSERT( !intersects, actor_blacklist_exception,
                     "authorizing actor(s) in transaction are on the actor blacklist: ${actors}",
                     ("actors", generate_blacklisted_actors(actors, blacklist))
                   );
      }
   }

   void check_contract_list( account_name code )const {
      if( conf.contract_whitelist.size() > 0 ) {
         EOS_ASSERT( conf.contract_whitelist.find( code ) != conf.contract_whitelist.end(),
                     contract_whitelist_exception,
                     "account '${code}' is not on the contract whitelist", ("code", code)
                   );
      } else if( conf.contract_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.contract_blacklist.find( code ) == conf.contract_blacklist.end(),
                     contract_blacklist_exception,
                     "account '${code}' is on the contract blacklist", ("code", code)
                   );
      }
   }

   void check_action_list( account_name code, action_name action )const {
      if( conf.action_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.action_blacklist.find( std::make_pair(code, action) ) == conf.action_blacklist.end(),
                     action_blacklist_exception,
                     "action '${code}::${action}' is on the action blacklist",
                     ("code", code)("action", action)
                   );
      }
   }

   void check_key_list( const public_key_type& key )const {
      if( conf.key_blacklist.size() > 0 ) {
         EOS_ASSERT( conf.key_blacklist.find( key ) == conf.key_blacklist.end(),
                     key_blacklist_exception,
                     "public key '${key}' is on the key blacklist",
                     ("key", key)
                   );
      }
   }

   /*
   bool should_check_tapos()const { return true; }

   void validate_tapos( const transaction& trx )const {
      if( !should_check_tapos() ) return;

      const auto& tapos_block_summary = db.get<block_summary_object>((uint16_t)trx.ref_block_num);

      //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
      EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
                 "Transaction's reference block did not match. Is this transaction from a different fork?",
                 ("tapos_summary", tapos_block_summary));
   }
   */


   /**
    *  At the start of each block we notify the system contract with a transaction that passes in
    *  the block header of the prior block (which is currently our head block)
    */
   signed_transaction get_on_block_transaction()
   {
      action on_block_act;
      on_block_act.account = config::system_account_name;
      on_block_act.name = N(onblock);
      on_block_act.authorization = vector<permission_level>{{config::system_account_name, config::active_name}};
      on_block_act.data = fc::raw::pack(self.head_block_header());

      signed_transaction trx;
      trx.actions.emplace_back(std::move(on_block_act));
      trx.set_reference_block(self.head_block_id());
      trx.expiration = self.pending_block_time() + fc::microseconds(999'999); // Round up to nearest second to avoid appearing expired
      return trx;
   }

   signed_transaction get_on_fee_transaction( const asset &fee, const account_name &actor)
   {
      action on_fee_act;
      on_fee_act.account = config::system_account_name;
      on_fee_act.name = N(onfee);
      on_fee_act.authorization = vector<permission_level>{{actor, config::active_name}};

      fee_paramter param(actor, fee, self.head_block_header().producer);
      on_fee_act.data = fc::raw::pack(param);

      signed_transaction trx;
      trx.actions.emplace_back(std::move(on_fee_act));
      trx.set_reference_block(self.head_block_id());
      trx.expiration = self.pending_block_time() + fc::microseconds(999'999); // Round up to nearest second to avoid appearing expired
      return trx;
   }
}; /// controller_impl

const resource_limits_manager&   controller::get_resource_limits_manager()const
{
   return my->resource_limits;
}
resource_limits_manager&         controller::get_mutable_resource_limits_manager()
{
   return my->resource_limits;
}

const authorization_manager&   controller::get_authorization_manager()const
{
   return my->authorization;
}
authorization_manager&         controller::get_mutable_authorization_manager()
{
   return my->authorization;
}

const txfee_manager&   controller::get_txfee_manager()const
{
   return my->txfee;
}
txfee_manager&         controller::get_mutable_txfee_manager()
{
   return my->txfee;
}

controller::controller( const controller::config& cfg )
:my( new controller_impl( cfg, *this ) )
{
}

controller::~controller() {
   my->abort_block();
   //close fork_db here, because it can generate "irreversible" signal to this controller,
   //in case if read-mode == IRREVERSIBLE, we will apply latest irreversible block
   //for that we need 'my' to be valid pointer pointing to valid controller_impl.
   my->fork_db.close();
}

void controller::add_indices() {
   my->add_indices();
}

void controller::startup( std::function<bool()> shutdown, const snapshot_reader_ptr& snapshot ) {
   my->head = my->fork_db.head();
   if( snapshot ) {
      ilog( "Starting initialization from snapshot, this may take a significant amount of time" );
   }
   else if( !my->head ) {
      elog( "No head block in fork db, perhaps we need to replay" );
   }

   try {
      my->init(shutdown, snapshot);
   } catch (boost::interprocess::bad_alloc& e) {
      if ( snapshot )
         elog( "db storage not configured to have enough storage for the provided snapshot, please increase and retry snapshot" );
      throw e;
   }
   if( snapshot ) {
      ilog( "Finished initialization from snapshot" );
   }
}

const chainbase::database& controller::db()const { return my->db; }

chainbase::database& controller::mutable_db()const { return my->db; }

const fork_database& controller::fork_db()const { return my->fork_db; }


void controller::start_block( block_timestamp_type when, uint16_t confirm_block_count) {
   validate_db_available_size();
   my->start_block(when, confirm_block_count, block_status::incomplete, optional<block_id_type>() );
}

void controller::finalize_block() {
   validate_db_available_size();
   my->finalize_block();
}

void controller::sign_block( const std::function<signature_type( const digest_type& )>& signer_callback ) {
   my->sign_block( signer_callback );
}

void controller::commit_block() {
   validate_db_available_size();
   validate_reversible_available_size();
   my->commit_block(true);
}

void controller::abort_block() {
   my->abort_block();
}

boost::asio::thread_pool& controller::get_thread_pool() {
   return my->thread_pool;
}

std::future<block_state_ptr> controller::create_block_state_future( const signed_block_ptr& b ) {
   return my->create_block_state_future( b );
}

void controller::push_block( std::future<block_state_ptr>& block_state_future ) {
   validate_db_available_size();
   validate_reversible_available_size();
   my->push_block( block_state_future );
}

transaction_trace_ptr controller::push_transaction( const transaction_metadata_ptr& trx, fc::time_point deadline, uint32_t billed_cpu_time_us ) {
   validate_db_available_size();
   EOS_ASSERT( get_read_mode() != chain::db_read_mode::READ_ONLY, transaction_type_exception, "push transaction not allowed in read-only mode" );
   EOS_ASSERT( trx && !trx->implicit && !trx->scheduled, transaction_type_exception, "Implicit/Scheduled transaction not allowed" );
   return my->push_transaction(trx, deadline, billed_cpu_time_us, billed_cpu_time_us > 0 );
}

transaction_trace_ptr controller::push_scheduled_transaction( const transaction_id_type& trxid, fc::time_point deadline, uint32_t billed_cpu_time_us )
{
   validate_db_available_size();
   return my->push_scheduled_transaction( trxid, deadline, billed_cpu_time_us, billed_cpu_time_us > 0 );
}

const flat_set<account_name>& controller::get_actor_whitelist() const {
   return my->conf.actor_whitelist;
}
const flat_set<account_name>& controller::get_actor_blacklist() const {
   return my->conf.actor_blacklist;
}
const flat_set<account_name>& controller::get_contract_whitelist() const {
   return my->conf.contract_whitelist;
}
const flat_set<account_name>& controller::get_contract_blacklist() const {
   return my->conf.contract_blacklist;
}
const flat_set< pair<account_name, action_name> >& controller::get_action_blacklist() const {
   return my->conf.action_blacklist;
}
const flat_set<public_key_type>& controller::get_key_blacklist() const {
   return my->conf.key_blacklist;
}

void controller::set_actor_whitelist( const flat_set<account_name>& new_actor_whitelist ) {
   my->conf.actor_whitelist = new_actor_whitelist;
}
void controller::set_actor_blacklist( const flat_set<account_name>& new_actor_blacklist ) {
   my->conf.actor_blacklist = new_actor_blacklist;
}
void controller::set_contract_whitelist( const flat_set<account_name>& new_contract_whitelist ) {
   my->conf.contract_whitelist = new_contract_whitelist;
}
void controller::set_contract_blacklist( const flat_set<account_name>& new_contract_blacklist ) {
   my->conf.contract_blacklist = new_contract_blacklist;
}
void controller::set_action_blacklist( const flat_set< pair<account_name, action_name> >& new_action_blacklist ) {
   for (auto& act: new_action_blacklist) {
      EOS_ASSERT(act.first != account_name(), name_type_exception, "Action blacklist - contract name should not be empty");
      EOS_ASSERT(act.second != action_name(), action_type_exception, "Action blacklist - action name should not be empty");
   }
   my->conf.action_blacklist = new_action_blacklist;
}
void controller::set_key_blacklist( const flat_set<public_key_type>& new_key_blacklist ) {
   my->conf.key_blacklist = new_key_blacklist;
}

uint32_t controller::head_block_num()const {
   return my->head->block_num;
}
time_point controller::head_block_time()const {
   return my->head->header.timestamp;
}
block_id_type controller::head_block_id()const {
   return my->head->id;
}
account_name  controller::head_block_producer()const {
   return my->head->header.producer;
}
const block_header& controller::head_block_header()const {
   return my->head->header;
}
block_state_ptr controller::head_block_state()const {
   return my->head;
}

uint32_t controller::fork_db_head_block_num()const {
   return my->fork_db.head()->block_num;
}

block_id_type controller::fork_db_head_block_id()const {
   return my->fork_db.head()->id;
}

time_point controller::fork_db_head_block_time()const {
   return my->fork_db.head()->header.timestamp;
}

account_name  controller::fork_db_head_block_producer()const {
   return my->fork_db.head()->header.producer;
}

block_state_ptr controller::pending_block_state()const {
   if( my->pending ) return my->pending->_pending_block_state;
   return block_state_ptr();
}
time_point controller::pending_block_time()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   return my->pending->_pending_block_state->header.timestamp;
}

optional<block_id_type> controller::pending_producer_block_id()const {
   EOS_ASSERT( my->pending, block_validate_exception, "no pending block" );
   return my->pending->_producer_block_id;
}

uint32_t controller::last_irreversible_block_num() const {
   return std::max(std::max(my->head->bft_irreversible_blocknum, my->head->dpos_irreversible_blocknum), my->snapshot_head_block);
}

block_id_type controller::last_irreversible_block_id() const {
   auto lib_num = last_irreversible_block_num();
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)lib_num);

   if( block_header::num_from_id(tapos_block_summary.block_id) == lib_num )
      return tapos_block_summary.block_id;

   return fetch_block_by_number(lib_num)->id();

}

const dynamic_global_property_object& controller::get_dynamic_global_properties()const {
  return my->db.get<dynamic_global_property_object>();
}
const global_property_object& controller::get_global_properties()const {
  return my->db.get<global_property_object>();
}

signed_block_ptr controller::fetch_block_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   if( state && state->block ) return state->block;
   auto bptr = fetch_block_by_number( block_header::num_from_id(id) );
   if( bptr && bptr->id() == id ) return bptr;
   return signed_block_ptr();
}

signed_block_ptr controller::fetch_block_by_number( uint32_t block_num )const  { try {
   auto blk_state = my->fork_db.get_block_in_current_chain_by_num( block_num );
   if( blk_state && blk_state->block ) {
      return blk_state->block;
   }

   return my->blog.read_block_by_num(block_num);
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_state_ptr controller::fetch_block_state_by_id( block_id_type id )const {
   auto state = my->fork_db.get_block(id);
   return state;
}

block_state_ptr controller::fetch_block_state_by_number( uint32_t block_num )const  { try {
   auto blk_state = my->fork_db.get_block_in_current_chain_by_num( block_num );
   return blk_state;
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

block_id_type controller::get_block_id_for_num( uint32_t block_num )const { try {
   auto blk_state = my->fork_db.get_block_in_current_chain_by_num( block_num );
   if( blk_state ) {
      return blk_state->id;
   }

   auto signed_blk = my->blog.read_block_by_num(block_num);

   EOS_ASSERT( BOOST_LIKELY( signed_blk != nullptr ), unknown_block_exception,
               "Could not find block: ${block}", ("block", block_num) );

   return signed_blk->id();
} FC_CAPTURE_AND_RETHROW( (block_num) ) }

sha256 controller::calculate_integrity_hash()const { try {
   return my->calculate_integrity_hash();
} FC_LOG_AND_RETHROW() }

void controller::write_snapshot( const snapshot_writer_ptr& snapshot ) const {
   EOS_ASSERT( !my->pending, block_validate_exception, "cannot take a consistent snapshot with a pending block" );
   return my->add_to_snapshot(snapshot);
}

void controller::pop_block() {
   my->pop_block();
}

int64_t controller::set_proposed_producers( vector<producer_key> producers ) {
   const auto& gpo = get_global_properties();
   auto cur_block_num = head_block_num() + 1;

   if( gpo.proposed_schedule_block_num.valid() ) {
      if( *gpo.proposed_schedule_block_num != cur_block_num )
         return -1; // there is already a proposed schedule set in a previous block, wait for it to become pending

      if( std::equal( producers.begin(), producers.end(),
                      gpo.proposed_schedule.producers.begin(), gpo.proposed_schedule.producers.end() ) )
         return -1; // the proposed producer schedule does not change
   }

   producer_schedule_type sch;

   decltype(sch.producers.cend()) end;
   decltype(end)                  begin;

   if( my->pending->_pending_block_state->pending_schedule.producers.size() == 0 ) {
      const auto& active_sch = my->pending->_pending_block_state->active_schedule;
      begin = active_sch.producers.begin();
      end   = active_sch.producers.end();
      sch.version = active_sch.version + 1;
   } else {
      const auto& pending_sch = my->pending->_pending_block_state->pending_schedule;
      begin = pending_sch.producers.begin();
      end   = pending_sch.producers.end();
      sch.version = pending_sch.version + 1;
   }

   if( std::equal( producers.begin(), producers.end(), begin, end ) )
      return -1; // the producer schedule would not change

   sch.producers = std::move(producers);

   int64_t version = sch.version;

   my->db.modify( gpo, [&]( auto& gp ) {
      gp.proposed_schedule_block_num = cur_block_num;
      gp.proposed_schedule = std::move(sch);
   });
   return version;
}

const producer_schedule_type&    controller::active_producers()const {
   if ( !(my->pending) )
      return  my->head->active_schedule;
   return my->pending->_pending_block_state->active_schedule;
}

const producer_schedule_type&    controller::pending_producers()const {
   if ( !(my->pending) )
      return  my->head->pending_schedule;
   return my->pending->_pending_block_state->pending_schedule;
}

optional<producer_schedule_type> controller::proposed_producers()const {
   const auto& gpo = get_global_properties();
   if( !gpo.proposed_schedule_block_num.valid() )
      return optional<producer_schedule_type>();

   return gpo.proposed_schedule;
}

bool controller::light_validation_allowed(bool replay_opts_disabled_by_policy) const {
   if (!my->pending || my->in_trx_requiring_checks) {
      return false;
   }

   const auto pb_status = my->pending->_block_status;

   // in a pending irreversible or previously validated block and we have forcing all checks
   const bool consider_skipping_on_replay = (pb_status == block_status::irreversible || pb_status == block_status::validated) && !replay_opts_disabled_by_policy;

   // OR in a signed block and in light validation mode
   const bool consider_skipping_on_validate = (pb_status == block_status::complete &&
         (my->conf.block_validation_mode == validation_mode::LIGHT || my->trusted_producer_light_validation));

   return consider_skipping_on_replay || consider_skipping_on_validate;
}


bool controller::skip_auth_check() const {
   return light_validation_allowed(my->conf.force_all_checks);
}

bool controller::skip_db_sessions( block_status bs ) const {
   bool consider_skipping = bs == block_status::irreversible;
   return consider_skipping
      && !my->conf.disable_replay_opts
      && !my->in_trx_requiring_checks;
}

bool controller::skip_db_sessions( ) const {
   if (my->pending) {
      return skip_db_sessions(my->pending->_block_status);
   } else {
      return false;
   }
}

bool controller::skip_trx_checks() const {
   return light_validation_allowed(my->conf.disable_replay_opts);
}

bool controller::contracts_console()const {
   return my->conf.contracts_console;
}

chain_id_type controller::get_chain_id()const {
   return my->chain_id;
}

db_read_mode controller::get_read_mode()const {
   return my->read_mode;
}

validation_mode controller::get_validation_mode()const {
   return my->conf.block_validation_mode;
}

const apply_handler* controller::find_apply_handler( account_name receiver, account_name scope, action_name act ) const
{
   auto native_handler_scope = my->apply_handlers.find( receiver );
   if( native_handler_scope != my->apply_handlers.end() ) {
      auto handler = native_handler_scope->second.find( make_pair( scope, act ) );
      if( handler != native_handler_scope->second.end() )
         return &handler->second;
   }
   return nullptr;
}
wasm_interface& controller::get_wasm_interface() {
   return my->wasmif;
}

const account_object& controller::get_account( account_name name )const
{ try {
   return my->db.get<account_object, by_name>(name);
} FC_CAPTURE_AND_RETHROW( (name) ) }

unapplied_transactions_type& controller::get_unapplied_transactions() {
   if ( my->read_mode != db_read_mode::SPECULATIVE ) {
      EOS_ASSERT( my->unapplied_transactions.empty(), transaction_exception,
                  "not empty unapplied_transactions in non-speculative mode" ); //should never happen
   }
   return my->unapplied_transactions;
}

bool controller::sender_avoids_whitelist_blacklist_enforcement( account_name sender )const {
   return my->sender_avoids_whitelist_blacklist_enforcement( sender );
}

void controller::check_actor_list( const flat_set<account_name>& actors )const {
   my->check_actor_list( actors );
}

void controller::check_contract_list( account_name code )const {
   my->check_contract_list( code );
}

void controller::check_action_list( account_name code, action_name action )const {
   my->check_action_list( code, action );
}

void controller::check_key_list( const public_key_type& key )const {
   my->check_key_list( key );
}

bool controller::is_producing_block()const {
   if( !my->pending ) return false;

   return (my->pending->_block_status == block_status::incomplete);
}

bool controller::is_ram_billing_in_notify_allowed()const {
   return !is_producing_block() || my->conf.allow_ram_billing_in_notify;
}

void controller::validate_expiration( const transaction& trx )const { try {
   const auto& chain_configuration = get_global_properties().configuration;

   EOS_ASSERT( time_point(trx.expiration) >= pending_block_time(),
               expired_tx_exception,
               "transaction has expired, "
               "expiration is ${trx.expiration} and pending block time is ${pending_block_time}",
               ("trx.expiration",trx.expiration)("pending_block_time",pending_block_time()));
   EOS_ASSERT( time_point(trx.expiration) <= pending_block_time() + fc::seconds(chain_configuration.max_transaction_lifetime),
               tx_exp_too_far_exception,
               "Transaction expiration is too far in the future relative to the reference time of ${reference_time}, "
               "expiration is ${trx.expiration} and the maximum transaction lifetime is ${max_til_exp} seconds",
               ("trx.expiration",trx.expiration)("reference_time",pending_block_time())
               ("max_til_exp",chain_configuration.max_transaction_lifetime) );
} FC_CAPTURE_AND_RETHROW((trx)) }

void controller::validate_tapos( const transaction& trx )const { try {
   const auto& tapos_block_summary = db().get<block_summary_object>((uint16_t)trx.ref_block_num);

   //Verify TaPoS block summary has correct ID prefix, and that this block's time is not past the expiration
   EOS_ASSERT(trx.verify_reference_block(tapos_block_summary.block_id), invalid_ref_block_exception,
              "Transaction's reference block did not match. Is this transaction from a different fork?",
              ("tapos_summary", tapos_block_summary));
} FC_CAPTURE_AND_RETHROW() }

void controller::validate_db_available_size() const {
   const auto free = db().get_segment_manager()->get_free_memory();
   const auto guard = my->conf.state_guard_size;
   EOS_ASSERT(free >= guard, database_guard_exception, "database free: ${f}, guard size: ${g}", ("f", free)("g",guard));
}

void controller::validate_reversible_available_size() const {
   const auto free = my->reversible_blocks.get_segment_manager()->get_free_memory();
   const auto guard = my->conf.reversible_guard_size;
   EOS_ASSERT(free >= guard, reversible_guard_exception, "reversible free: ${f}, guard size: ${g}", ("f", free)("g",guard));
}

bool controller::is_known_unexpired_transaction( const transaction_id_type& id) const {
   return db().find<transaction_object, by_trx_id>(id);
}

void controller::set_subjective_cpu_leeway(fc::microseconds leeway) {
   my->subjective_cpu_leeway = leeway;
}

void controller::add_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.insert(name);
}

void controller::remove_resource_greylist(const account_name &name) {
   my->conf.resource_greylist.erase(name);
}

bool controller::is_resource_greylisted(const account_name &name) const {
   return my->conf.resource_greylist.find(name) !=  my->conf.resource_greylist.end();
}

const flat_set<account_name> &controller::get_resource_greylist() const {
   return  my->conf.resource_greylist;
}

// format_name format name from genesis
const std::string format_name( const std::string& name ) {
   std::stringstream ss;
   for( int i = 0; i < 12; i++ ) {
      const auto n = name[i];
      if( n >= 'A' && n <= 'Z' ) {
         ss << static_cast<char>( n + 32 );
      } else if(( n >= 'a' && n <= 'z' ) || ( n >= '1' && n <= '5' )) {
         ss << static_cast<char>( n );
      } else if( n >= '6' && n <= '9' ) {
         ss << static_cast<char>( n - 5 );
      } else {
         // unknown char no process
      }
   }

   const auto res = ss.str();

   if( res.size() < 12 ) {
      EOS_ASSERT(false, name_type_exception, "initialize format name failed");
   }
   return res;
}

} } /// eosio::chain
