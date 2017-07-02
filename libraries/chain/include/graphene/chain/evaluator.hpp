/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once
#include <graphene/chain/exceptions.hpp>
#include <graphene/chain/transaction_evaluation_state.hpp>
#include <graphene/chain/protocol/operations.hpp>

namespace graphene { namespace chain {

   class database;
   struct signed_transaction;
   class generic_evaluator;
   class transaction_evaluation_state;

   class generic_evaluator
   {
   public:
      virtual ~generic_evaluator(){}

      virtual int get_type()const = 0;
      virtual operation_result start_evaluate(transaction_evaluation_state& eval_state, const operation& op, bool apply);

      /**
       * @note derived classes should ASSUME that the default validation that is
       * indepenent of chain state should be performed by op.validate() and should
       * not perform these extra checks.
       */
      virtual operation_result evaluate(const operation& op) = 0;
      virtual operation_result apply(const operation& op) = 0;

      /**
       * Routes the fee to where it needs to go.  The default implementation
       * routes the fee to the account_statistics_object of the fee_paying_account.
       *
       * Before pay_fee() is called, the fee is computed by prepare_fee() and has been
       * moved out of the fee_paying_account and (if paid in a non-CORE asset) converted
       * by the asset's fee pool.
       *
       * Therefore, when pay_fee() is called, the fee only exists in this->core_fee_paid.
       * So pay_fee() need only increment the receiving balance.
       *
       * The default implementation simply calls account_statistics_object->pay_fee() to
       * increment pending_fees or pending_vested_fees.
       */
      virtual void pay_fee();

      database& db()const;

      //void check_required_authorities(const operation& op);
   protected:
      /**
       * @brief Fetch objects relevant to fee payer and set pointer members
       * @param account_id Account which is paying the fee
       * @param fee The fee being paid. May be in assets other than core.
       *
       * This method verifies that the fee is valid and sets the object pointer members and the fee fields. It should
       * be called during do_evaluate.
       *
       * In particular, core_fee_paid field is set by prepare_fee().
       */
      void prepare_fee(account_id_type account_id, asset fee);
      void prepare_fee(account_uid_type account_uid, asset fee);
      void prepare_fee(asset fee); // be called after fee_paying_account initialized
      void prepare_fee(account_uid_type account_uid, const fee_type& fee);
      void prepare_fee(const fee_type& fee); // be called after fee_paying_account initialized

      /**
       * Convert the fee into BTS through the exchange pool.
       *
       * Reads core_fee_paid field for how much CORE is deducted from the exchange pool,
       * and fee_from_account for how much USD is added to the pool.
       *
       * Since prepare_fee() does the validation checks ensuring the account and fee pool
       * have sufficient balance and the exchange rate is correct,
       * those validation checks are not replicated here.
       *
       * Rather than returning a value, this method fills in core_fee_paid field.
       */
      void convert_fee();

      object_id_type get_relative_id( object_id_type rel_id )const;

      /**
       * pay_fee() for FBA subclass should simply call this method
       */
      void pay_fba_fee( uint64_t fba_id );

      /**
       * Process fee_options.
       */
      void process_fee_options();

      /**
       * Calculate fee for an operation.
       */
      share_type calculate_fee_for_operation(const operation& op) const;

      /**
       * Calculate fee pair for an operation.
       * @return A pair, the first is total fee required, the second is minimum non-csaf fee required
       */
      std::pair<share_type,share_type> calculate_fee_pair_for_operation(const operation& op) const;

      // the next two functions are helpers that allow template functions declared in this 
      // header to call db() without including database.hpp, which would
      // cause a circular dependency
      void db_adjust_balance(const account_id_type& fee_payer, asset fee_from_account);
      void db_adjust_balance(const account_uid_type& fee_payer, asset fee_from_account);
      string db_to_pretty_string( const asset& a )const;
      string db_to_pretty_core_string( const share_type amount )const;

      asset                            fee_from_account;
      share_type                       core_fee_paid;
      share_type                       total_fee_paid;
      share_type                       from_balance;
      share_type                       from_prepaid;
      share_type                       from_csaf;
      const account_object*            fee_paying_account = nullptr;
      const account_statistics_object* fee_paying_account_statistics = nullptr;
      const asset_object*              fee_asset          = nullptr;
      const asset_dynamic_data_object* fee_asset_dyn_data = nullptr;
      transaction_evaluation_state*    trx_state;
   };

   class op_evaluator
   {
   public:
      virtual ~op_evaluator(){}
      virtual operation_result evaluate(transaction_evaluation_state& eval_state, const operation& op, bool apply) = 0;
   };

   template<typename T>
   class op_evaluator_impl : public op_evaluator
   {
   public:
      virtual operation_result evaluate(transaction_evaluation_state& eval_state, const operation& op, bool apply = true) override
      {
         T eval;
         return eval.start_evaluate(eval_state, op, apply);
      }
   };

   template<typename DerivedEvaluator>
   class evaluator : public generic_evaluator
   {
   public:
      virtual int get_type()const override { return operation::tag<typename DerivedEvaluator::operation_type>::value; }

      virtual operation_result evaluate(const operation& o) final override
      {
         auto* eval = static_cast<DerivedEvaluator*>(this);
         const auto& op = o.get<typename DerivedEvaluator::operation_type>();

         prepare_fee(op.fee_payer_uid(), op.fee);
         if( !trx_state->skip_fee_schedule_check )
         {
            std::pair<share_type,share_type> required_fee_pair = calculate_fee_pair_for_operation(op);
            GRAPHENE_ASSERT( total_fee_paid >= required_fee_pair.first,
                       insufficient_fee,
                       "Insufficient Total Fee Paid: need ${r}, provided ${p}",
                       ("p",db_to_pretty_core_string(total_fee_paid))
                       ("r",db_to_pretty_core_string(required_fee_pair.first)) );
            GRAPHENE_ASSERT( from_balance + from_prepaid >= required_fee_pair.second,
                       insufficient_fee,
                       "Insufficient Real Fee Paid: need ${r}, provided ${fb} from balance and ${fp} from prepaid",
                       ("fb",db_to_pretty_core_string(from_balance))
                       ("fp",db_to_pretty_core_string(from_prepaid))
                       ("r" ,db_to_pretty_core_string(required_fee_pair.second)) );
         }

         return eval->do_evaluate(op);
      }

      virtual operation_result apply(const operation& o) final override
      {
         auto* eval = static_cast<DerivedEvaluator*>(this);
         const auto& op = o.get<typename DerivedEvaluator::operation_type>();

         convert_fee();
         pay_fee();

         auto result = eval->do_apply(op);

         if( fee_from_account.amount > 0 )
            db_adjust_balance(op.fee_payer_uid(), -fee_from_account);

         process_fee_options();

         return result;
      }
   };
} }
