<?php

/**
 * @file
 * Front-end form pre-processor.
 */

module_load_include('inc', 'webform_civicrm', 'includes/wf_crm_webform_base');

class wf_crm_webform_preprocess extends wf_crm_webform_base {
  private $form;
  private $form_state;
  private $info = [];
  private $all_fields;
  private $all_sets;

  function __construct(&$form, &$form_state) {
    civicrm_initialize();
    $this->form = &$form;
    $this->form_state = &$form_state;
    $this->node = $form['#node'];
    $this->settings = $this->node->webform_civicrm;
    $this->data = $this->settings['data'];
    $this->fixPaymentProcessorID();
    $this->ent['contact'] = [];
    $this->all_fields = wf_crm_get_fields();
    $this->all_sets = wf_crm_get_fields('sets');
    $this->enabled = wf_crm_enabled_fields($this->node);
    $this->line_items = wf_crm_aval($form_state, 'civicrm:line_items', []);
  }

  /**
   * Alter front-end of webforms: Called by hook_form_alter() when rendering a civicrm-enabled webform
   * Add custom prefix.
   * Display messages.
   * Block users who should not have access.
   * Set webform default values.
   */
  public function alterForm() {
    // Add css & js
    $this->addResources();
    // Add validation handler
    $this->form['#validate'][] = 'wf_crm_validate';
    // If this form is already in-process, IDs will be stored
    if (!empty($this->form_state['civicrm'])) {
      $this->ent = $this->form_state['civicrm']['ent'];
    }
    $submitted_contacts = [];
    // Keep track of cids across multipage forms
    if (!empty($this->form_state['values']['submitted']) && wf_crm_aval($this->form_state, 'webform:page_count') > 1) {
      foreach ($this->enabled as $k => $v) {
        if (substr($k, -8) == 'existing' && isset($this->form_state['values']['submitted'][$v])) {
          list(, $c) = explode('_', $k);
          $val = $this->form_state['values']['submitted'][$v];
          $cid_data["cid$c"] = $this->ent['contact'][$c]['id'] = (int) (is_array($val) ? $val[0] : $val);
          $submitted_contacts[$c] = TRUE;
        }
      }
      if (!empty($cid_data)) {
        $this->form['#attributes']['data-civicrm-ids'] = json_encode($cid_data);
      }
    }
    // Early return if the form (or page) was already submitted
    if (wf_crm_aval($this->form_state, 'triggering_element:#id') == 'edit-previous'
      || (empty($this->form_state['rebuild']) && !empty($this->form_state['storage']) && empty($this->form['#submission']->is_draft))
      // When resuming from a draft
      || (!empty($this->form['#submission']->is_draft) && empty($this->form_state['input']))
    ) {
      $this->fillForm($this->form['submitted']);
      return;
    }
    // If this is an edit op, use the original IDs and return
    if (isset($this->form['#submission']->sid) && $this->form['#submission']->is_draft != '1') {
      if (isset($this->form['#submission']->civicrm)) {
        $this->form_state['civicrm']['ent'] = $this->form['#submission']->civicrm;
        foreach ($this->form_state['civicrm']['ent']['contact'] as $c => $contact) {
          $this->info['contact'][$c]['contact'][1]['existing'] = wf_crm_aval($contact, 'id', 0);
        }
      }
      $this->fillForm($this->form['submitted']);
      return;
    }
    // Search for existing contacts
    for ($c = 1; $c <= count($this->data['contact']); ++$c) {
      $this->ent['contact'][$c] = wf_crm_aval($this->ent, "contact:$c", []);
      $existing_component = $this->getComponent("civicrm_{$c}_contact_1_contact_existing");
      // Search for contact if the user hasn't already chosen one
      if ($existing_component && empty($submitted_contacts[$c])) {
        $this->findContact($existing_component);
      }
      // Fill cid with '0' if unknown
      $this->ent['contact'][$c] += ['id' => 0];
    }
    // Search for other existing entities
    if (empty($this->form_state['civicrm'])) {
      if (!empty($this->data['case']['number_of_case'])) {
        $this->findExistingCases();
      }
      if (!empty($this->data['activity']['number_of_activity'])) {
        $this->findExistingActivities();
      }
      if (isset($this->data['grant']['number_of_grant'])) {
        $this->findExistingGrants();
      }
    }
    // Check if contact reload required: for case roles
    for ($c = 1; $c <= count($this->data['contact']); ++$c) {
      if (isset($this->ent['contact'][$c]['reload'])) {
        $existing_component = $this->getComponent("civicrm_{$c}_contact_1_contact_existing");
        $this->ent['contact'][$c]['id'] = 0;
        $this->findContact($existing_component);
      }
    }
    // Form alterations for unknown contacts
    if (empty($this->ent['contact'][1]['id'])) {
      if ($this->settings['prefix_unknown']) {
        $this->form['#prefix'] = wf_crm_aval($this->form, '#prefix', '') . '<div class="webform-civicrm-prefix contact-unknown">' . nl2br($this->settings['prefix_unknown']) . '</div>';
      }
      if ($this->settings['block_unknown_users']) {
        $this->form['submitted']['#access'] = $this->form['actions']['#access'] = FALSE;
        $this->setMessage(t('Sorry, you do not have permission to access this form.'), 'warning');
        return;
      }
    }
    if (!empty($this->data['participant_reg_type'])) {
      $this->populateEvents();
    }
    // Form alterations for known contacts
    foreach ($this->ent['contact'] as $c => $contact) {
      if (!empty($contact['id'])) {
        // Retrieve contact data
        $this->info['contact'][$c] = $this->loadContact($c);
        $this->info['contact'][$c]['contact'][1]['existing'] = $contact['id'];
        // Retrieve participant data
        if ($this->events && ($c == 1 || $this->data['participant_reg_type'] == 'separate')) {
          $this->loadParticipants($c);
        }
        // Membership
        if (!empty($this->data['membership'][$c]['number_of_membership'])) {
          $this->loadMemberships($c, $contact['id']);
        }
      }
      // Load events from url if enabled, this will override loadParticipants
      if (!empty($this->data['reg_options']['allow_url_load'])) {
        $this->loadURLEvents($c);
      }
    }
    // Prefill other existing entities
    foreach (['case', 'activity', 'grant'] as $entity) {
      if (!empty($this->ent[$entity])) {
        $this->populateExistingEntity($entity);
      }
    }
    if (!empty($this->ent['contact'][1]['id'])) {
      if ($this->settings['prefix_known']) {
        $this->form['#prefix'] = wf_crm_aval($this->form, '#prefix', '') . '<div class="webform-civicrm-prefix contact-known">' . nl2br($this->replaceTokens($this->settings['prefix_known'], $this->info['contact'][1]['contact'][1])) . '</div>';
      }
      if ($this->settings['message']) {
        $this->showNotYouMessage($this->settings['message'], $this->info['contact'][1]['contact'][1]);
      }
    }
    // Store ids
    $this->form_state['civicrm']['ent'] = $this->ent;
    // Set default values and other attributes for CiviCRM form elements
    // Passing $submitted helps avoid overwriting values that have been entered on a multi-step form
    $submitted = wf_crm_aval($this->form_state, 'values:submitted', []);
    $this->fillForm($this->form['submitted'], $submitted);
  }

  /**
   * Add necessary js & css to the form
   */
  private function addResources() {
    $this->form['#attached']['js'][] = [
      'data' => drupal_get_path('module', 'webform_civicrm') . '/js/webform_civicrm_forms.js',
      'scope' => 'footer',
    ];
    $this->form['#attached']['css'][] = drupal_get_path('module', 'webform_civicrm') . '/css/webform_civicrm_forms.css';
    $default_country = wf_crm_get_civi_setting('defaultContactCountry', 1228);
    // Variables to push to the client-side
    $js_vars = [];
    // JS Cache eliminates the need for most ajax state/province callbacks
    foreach ($this->data['contact'] as $c) {
      if (!empty($c['number_of_address'])) {
        $js_vars += [
          'defaultCountry' => $default_country,
          'defaultStates' => wf_crm_get_states($default_country),
          'noCountry' => t('- First Choose a Country -'),
          'callbackPath' => url('webform-civicrm/js', ['alias' => TRUE]),
        ];
        break;
      }
    }
    // Preprocess contribution page
    if (!empty($this->data['contribution'])) {
      $this->addPaymentJs();
      $this->form['#attached']['js'][] = [
        'data' => drupal_get_path('module', 'webform_civicrm') . '/js/webform_civicrm_payment.js',
        'scope' => 'footer',
      ];
      $currency = $this->contribution_page['currency'];
      $contributionCallbackQuery = ['currency' => $currency, 'snippet' => 4, 'is_drupal_webform' => 1];
      $contributionCallbackUrl = 'civicrm/payment/form';
      $js_vars['processor_id_key'] = 'processor_id';
      if (!empty($this->data['contribution'][1]['contribution'][1]['is_test'])) {
        // RM: This is needed in order for CiviCRM to know that this is a 'test' (i.e. 'preview' action in CiviCRM) transaction - otherwise, CiviCRM defaults to 'live' and returns the payment form with public key for the live payment processor!
        $contributionCallbackQuery['action'] = CRM_Core_Action::description(CRM_Core_Action::PREVIEW);
      }
      $js_vars['contributionCallback'] = url($contributionCallbackUrl, ['query' => $contributionCallbackQuery, 'alias' => TRUE]);
      // Add payment processor - note we have to search in 2 places because $this->loadMultipageData hasn't been run. Maybe it should be?
      $fid = 'civicrm_1_contribution_1_contribution_payment_processor_id';
      if (!empty($this->enabled[$fid])) {
        $js_vars['paymentProcessor'] = wf_crm_aval($this->form_state, 'storage:submitted:' . $this->enabled[$fid]);
      }
      else {
        $js_vars['paymentProcessor'] = $this->getData($fid);
      }
    }
    if ($js_vars) {
      $this->form['#attached']['js'][] = [
        'data' => ['webform_civicrm' => $js_vars],
        'type' => 'setting',
      ];
    }
  }

  /**
   * Check if events are open to registration and take appropriate action
   */
  private function populateEvents() {
    $reg = wf_crm_aval($this->data, 'reg_options', []);
    // Fetch events set in back-end
    $this->data += ['participant' => []];
    foreach ($this->data['participant'] as $e => $par) {
      if (!empty($par['participant'])) {
        foreach ($par['participant'] as $n => $p) {
          if (!empty($p['event_id'])) {
            // Handle multi-valued event selection
            foreach ((array) $p['event_id'] as $eid) {
              if ($eid = (int) $eid) {
                $this->events[$eid]['ended'] = TRUE;
                $this->events[$eid]['title'] = t('this event');
                $this->events[$eid]['count'] = wf_crm_aval($this->events, "$eid:count", 0) + 1;
                $status_fid = "civicrm_{$e}_participant_{$n}_participant_status_id";
                $this->events[$eid]['form'][] = [
                  'contact' => $e,
                  'num' => $n,
                  'eid' => NULL,
                  'status_id' => (array) $this->getData($status_fid, array_keys($this->getExposedOptions($status_fid))),
                ];
              }
            }
          }
        }
      }
    }
    // Add events exposed to the form
    foreach ($this->enabled as $field => $fid) {
      if (strpos($field, 'participant_event_id')) {
        foreach ($this->getExposedOptions($field) as $p => $label) {
          list($eid) = explode('-', $p);
          $this->events[$eid]['ended'] = TRUE;
          $this->events[$eid]['title'] = $label;
          list(, $e, , $n) = explode('_', $field);
          $status_fid = "civicrm_{$e}_participant_{$n}_participant_status_id";
          $this->events[$eid]['form'][] = [
            'contact' => $e,
            'num' => $n,
            'eid' => $p,
            'status_id' => (array) $this->getData($status_fid, array_keys($this->getExposedOptions($status_fid))),
          ];
        }
      }
    }
    if ($this->events && (!empty($reg['show_remaining']) || !empty($reg['block_form']))) {
      $this->loadEvents();
      foreach ($this->events as $eid => $event) {
        if ($event['ended']) {
          if (!empty($reg['show_remaining'])) {
            $this->setMessage(t('Sorry, %event has ended.', ['%event' => $event['title']]), 'warning');
          }
        }
        elseif (!empty($event['is_full'])) {
          if (!empty($reg['show_remaining'])) {
            $this->setMessage('<em>' . check_plain($event['title']) . '</em>: ' . check_plain($event['event_full_text']), 'warning');
          }
        }
        else {
          $reg['block_form'] = FALSE;
          if (!empty($event['max_participants']) && ($reg['show_remaining'] == 'always' || intval($reg['show_remaining']) >= $event['available_places'])) {
            $this->setMessage(format_plural($event['available_places'],
              '%event has 1 remaining space.',
              '%event has @count remaining spaces.',
              ['%event' => $event['title']]));
          }
        }
      }
      if ($reg['block_form']) {
        $this->form['submitted']['#access'] = $this->form['actions']['#access'] = FALSE;
        return;
      }
    }
  }

  /**
   * Load participant data for a contact
   * @param int $c
   */
  private function loadParticipants($c) {
    $status_types = wf_crm_apivalues('participant_status_type', 'get');
    $participants = wf_crm_apivalues('participant', 'get', [
      'contact_id' => $this->ent['contact'][$c]['id'],
      'event_id' => ['IN' => array_keys($this->events)],
    ]);
    foreach ($participants as $row) {
      $par = [];
      // v3 participant api returns some non-standard keys with participant_ prepended
      foreach (['id', 'event_id', 'role_id', 'status_id', 'campaign_id'] as $sel) {
        $par['participant'][1][$sel] = $row[$sel] = $row[$sel] ?? $row['participant_' . $sel] ?? NULL;
      }
      $par += $this->getCustomData($row['id'], 'Participant');
      $status = $status_types[$row['status_id']];
      foreach ($this->events[$row['event_id']]['form'] as $event) {
        if ($event['contact'] == $c) {
          // If status has been set by admin or exposed to the form, use it as a filter
          if (in_array($status['id'], $event['status_id']) ||
            // If status is "Automatic" (empty) then make sure the participant is registered
            (empty($event['status_id']) && $status['class'] != 'Negative')
          ) {
            $n = $event['contact'];
            $i = $event['num'];
            // Support multi-valued form elements as best we can
            $event_ids = wf_crm_aval($this->info, "participant:$n:participant:$i:event_id", []);
            if ($event['eid']) {
              $event_ids[] = $event['eid'];
            }
            foreach ($par as $k => $v) {
              $this->info['participant'][$n][$k][$i] = $v[1];
            }
            $this->info['participant'][$n]['participant'][$i]['event_id'] = $event_ids;
          }
        }
      }
    }
  }

  /**
   * Load event data for the url
   * @param int $c
   */
  private function loadURLEvents($c) {
    $n = $this->data['participant_reg_type'] == 'separate' ? $c : 1;
    $p = wf_crm_aval($this->data, "participant:$n:participant");
    if ($p) {
      foreach ($p as $e => $value) {
        $event_ids = [];
        // Get the available event list from the component
        $fid = "civicrm_{$c}_participant_{$e}_participant_event_id";
        $eids = [];
        foreach ($this->getExposedOptions($fid) as $eid => $title) {
          $id = explode('-', $eid);
          $eids[$id[0]] = $eid;
        }
        if ($this->data['participant_reg_type'] == 'all') {
          $urlParam = "event$e";
        }
        else {
          $urlParam = "c{$c}event{$e}";
        }
        foreach (explode(',', wf_crm_aval($_GET, $urlParam)) as $value) {
          if (isset($eids[$value])){
            $event_ids[] = $eids[$value];
          }
        }
        $this->info['participant'][$c]['participant'][$e]['event_id'] = $event_ids;
      }
    }
  }

  /**
   * Load existing membership information and display a message to members.
   * @param int $c
   * @param int $cid
   */
  private function loadMemberships($c, $cid) {
    $today = date('Y-m-d');
    foreach ($this->findMemberships($cid) as $num => $membership) {
      // Only show 1 expired membership, and only if there are no active ones
      if (!$membership['is_active'] && $num) {
        break;
      }
      $type = $membership['membership_type_id'];
      if(empty($this->settings["membership_hide_status_msg"])){
        $msg = t('@type membership for @contact has a status of "@status".', [
          '@type' => $this->getMembershipTypeField($type, 'name'),
          '@contact' => $this->info['contact'][$c]['contact'][1]['display_name'],
          '@status' => $membership['status'],
        ]);
        if (!empty($membership['end_date'])) {
          $end = ['@date' => CRM_Utils_Date::customFormat($membership['end_date'])];
          $msg .= ' ' . ($membership['end_date'] > $today ? t('Expires @date.', $end) : t('Expired @date.', $end));
        }
        $this->setMessage($msg);
      }
      for ($n = 1; $n <= $this->data['membership'][$c]['number_of_membership']; ++$n) {
        $fid = "civicrm_{$c}_membership_{$n}_membership_membership_type_id";
        if (empty($info['membership'][$c]['membership'][$n]) && ($this->getData($fid) == $type ||
            array_key_exists($type, $this->getExposedOptions($fid)))
        ) {
          $this->info['membership'][$c]['membership'][$n] = $membership;
          break;
        }
      }
    }
  }

  /**
   * Recursively walk through form array and set properties of CiviCRM fields
   *
   * @param array $elements (reference)
   *   FAPI form array
   * @param array $submitted
   *   Existing submission (optional)
   */
  private function fillForm(&$elements, $submitted = []) {
    foreach ($elements as $eid => &$element) {
      if ($eid[0] == '#' || !is_array($element)) {
        continue;
      }
      // Recurse through nested elements
      $this->fillForm($element, $submitted);
      if (empty($element['#type']) || $element['#type'] == 'fieldset') {
        continue;
      }
      if (!empty($element['#webform_component']) && $pieces = wf_crm_explode_key($eid)) {
        list( , $c, $ent, $n, $table, $name) = $pieces;
        // Separate out time fields
        if (substr($name, -8) === 'timepart') {
          $name = str_replace('_timepart', '', $name);
        }
        if ($field = wf_crm_aval($this->all_fields, $table . '_' . $name)) {
          $component = $element['#webform_component'];
          $element['#attributes']['class'][] = 'civicrm-enabled';
          $dt = NULL;
          if (!empty($field['data_type'])) {
            $dt = $element['#civicrm_data_type'] = $field['data_type'];
          }
          $element['#attributes']['data-civicrm-field-key'] = $eid;
          // For contribution line-items
          if ($table == 'contribution' && in_array($name, ['line_total', 'total_amount'])) {
            $element['#attributes']['class'][] = 'contribution-line-item';
          }
          // Provide live options from the Civi DB
          if (!empty($component['extra']['civicrm_live_options']) && isset($element['#options'])) {
            $params = ['extra' => wf_crm_aval($field, 'extra', [])] + $component;
            $new = wf_crm_field_options($params, 'live_options', $this->data);
            $old = $element['#options'];
            $resave = FALSE;
            // If an item doesn't exist, we add it. If it's changed, we update it.
            // But we don't subtract items that have been removed in civi - this prevents
            // breaking the display of old submissions.
            foreach ($new as $k => $v) {
              if (!isset($old[$k]) || $old[$k] != $v) {
                $old[$k] = $v;
                $resave = TRUE;
              }
            }
            if ($resave) {
              $component['extra']['items'] = wf_crm_array2str($old);
              webform_component_update($component);
            }
            $element['#options'] = $new;
          }
          // If the user has already entered a value for this field, don't change it
          if (isset($this->info[$ent][$c][$table][$n][$name])
            && !(isset($component['cid']) && isset($submitted[$component['cid']]))) {
            $val = $this->info[$ent][$c][$table][$n][$name];

            if ($ent === 'contact') {
              $createModeKey = 'civicrm_' . $c . '_contact_' . $n . '_' . $table . '_createmode';
              $multivaluesCreateMode = isset($this->data['config']['create_mode'][$createModeKey]) ? (int) $this->data['config']['create_mode'][$createModeKey] : NULL;

              // Set empty value for field with 'Create only' mode.
              if ($multivaluesCreateMode === self::MULTIVALUE_FIELDSET_MODE_CREATE_ONLY) {
                $val = wf_crm_aval($element, '#default_value', '');
              }
            }
            if (($element['#type'] == 'checkboxes' || !empty($element['#multiple'])) && !is_array($val)) {
              $val = wf_crm_explode_multivalue_str($val);
            }
            if ($element['#type'] != 'checkboxes' && $element['#type'] != 'date'
              && empty($element['#multiple']) && is_array($val)) {
              // If there's more than one value for a non-multi field, pick the most appropriate
              if (!empty($element['#options'])) {
                foreach ($element['#options'] as $k => $v) {
                  if (in_array($k, $val)) {
                    $val = $k;
                    break;
                  }
                }
              }
              else {
                $val = array_pop($val);
              }
            }
            if ($component['type'] == 'autocomplete' && is_string($val) && strlen($val)) {
              $options = wf_crm_field_options($component, '', $this->data);
              $val = wf_crm_aval($options, $val);
            }
            // Contact image & custom file fields
            if ($dt == 'File') {
              $fileInfo = $this->getFileInfo($name, $val, $ent, $n);
              if ($fileInfo && in_array($element['#type'], ['file', 'managed_file'])) {
                $fileInfo = json_encode($fileInfo);
                $js = "jQuery(function() {wfCivi.initFileField('$eid', $fileInfo)});";
                $element['#attached']['js'][$js] = ['type' => 'inline'];
              }
            }
            // Set value for "secure value" elements
            elseif ($element['#type'] == 'value') {
              $element['#value'] = $val;
            }
            // Set default value
            else {
              $element['#default_value'] = $val;
            }
          }
          if ($name == 'existing') {
            wf_crm_fill_contact_value($this->node, $component, $element, $this->ent);
          }
          if ($name == 'contribution_page_id') {
            $element['#prefix'] = $this->displayLineItems();
            $element['#suffix'] = '<div class="crm-container crm-public" id="billing-payment-block"></div>';
            $element['#value'] = wf_crm_aval($this->data, 'contribution:1:contribution:1:contribution_page_id');
            unset($element['#default_value']);
          }
        }
      }
    }
  }

  /**
   * Format line-items to appear on front-end of webform
   * @return string
   */
  private function displayLineItems() {
    $rows = [];
    $total = 0;
    // Support hidden contribution field
    $fid = 'civicrm_1_contribution_1_contribution_total_amount';
    if (!$this->line_items && isset($this->enabled[$fid])) {
      $field = $this->node->webform['components'][$this->enabled[$fid]];
      if ($field['type'] == 'hidden') {
        $this->line_items[] = [
          'line_total' => $field['value'],
          'qty' => 1,
          'element' => 'civicrm_1_contribution_1',
          'label' => !empty($field['name']) ? $field['name'] : t('Contribution Amount'),
        ];
      }
    }

    $taxRates = CRM_Core_PseudoConstant::getTaxRates();
    foreach ($this->line_items as $item) {
      $total += $item['line_total'];
      // Sales Tax integration
      if (!empty($item['financial_type_id'])) {
        $itemTaxRate = isset($taxRates[$item['financial_type_id']]) ? $taxRates[$item['financial_type_id']] : NULL;
      }
      else {
        $itemTaxRate = $this->tax_rate;
      }

      if (!is_null($itemTaxRate)) {
        // Change the line item label to display the tax rate it contains
        $taxSettings = wf_crm_get_civi_setting('contribution_invoice_settings');

        if (($taxSettings['tax_display_settings'] != 'Do_not_show') && ($itemTaxRate !== 0)) {
          $item['label'] .= ' (' . t('includes !rate @tax', ['!rate' => (float)$itemTaxRate . '%', '@tax' => $taxSettings['tax_term']]) . ')';
        }

        // Add calculation for financial type that contains tax
        $item['tax_amount'] = ($itemTaxRate / 100) * $item['line_total'];
        $total += $item['tax_amount'];
        $label = $item['label'] . ($item['qty'] > 1 ? " ({$item['qty']})" : '');
        $rows[] = [
          'data' => [$label, CRM_Utils_Money::format($item['line_total'] + $item['tax_amount'])],
          'class' => [$item['element'], 'line-item'],
          'data-amount' => $item['line_total'] + $item['tax_amount'],
          'data-tax' => (float)$itemTaxRate,
        ];
      }
      else {
        $label = $item['label'] . ($item['qty'] > 1 ? " ({$item['qty']})" : '');
        $rows[] = [
          'data' => [$label, CRM_Utils_Money::format($item['line_total'])],
          'class' => [$item['element'], 'line-item'],
          'data-amount' => $item['line_total'],
        ];
      }
    }
    $rows[] = [
      'data' => [t('Total'), CRM_Utils_Money::format($total)],
      'id' => 'wf-crm-billing-total',
      'data-amount' => $total,
    ];
    return theme('table', [
      'sticky' => FALSE,
      'caption' => $this->contribution_page['title'],
      'header' => [],
      'rows' => $rows,
      'attributes' => ['id' => "wf-crm-billing-items"],
    ]);
  }

  /**
   * Find case ids based on url input or "existing case" settings
   */
  private function findExistingCases() {
    // Support legacy url param
    if (empty($_GET["case1"]) && !empty($_GET["caseid"])) {
      $_GET["case1"] = $_GET["caseid"];
    }
    $allContacts = [];
    foreach ($this->ent['contact'] as $contact) {
      if (!empty($contact['id'])) {
        $allContacts[] = $contact['id'];
      }
    }
    for ($n = 1; $n <= $this->data['case']['number_of_case']; ++$n) {
      if (!empty($this->data['case'][$n]['case'][1]['client_id'])) {
        $clients = [];
        foreach ((array)$this->data['case'][$n]['case'][1]['client_id'] as $c) {
          if (!empty($this->ent['contact'][$c]['id'])) {
            $clients[] = $this->ent['contact'][$c]['id'];
          }
        }
        if ($clients || $allContacts) {
          // Populate via url argument
          if (isset($_GET["case$n"]) && wf_crm_is_positive($_GET["case$n"])) {
            $id = $_GET["case$n"];
            $item = wf_civicrm_api('case', 'getsingle', ['id' => $id]);
            if (array_intersect((array) wf_crm_aval($item, 'client_id'), $clients) || user_access('access all cases and activities')) {
              $this->ent['case'][$n] = ['id' => $id];
            }
            else {
              foreach ($item['contacts'] ?? [] as $caseContact) {
                if (!empty($caseContact['manager']) && in_array($caseContact['contact_id'], $allContacts)) {
                  $this->ent['case'][$n] = ['id' => $id];
                }
              }
            }
          }
          // Populate via search
          elseif (!empty($this->data['case'][$n]['existing_case_status'])) {
            $item = $this->findCaseForContact($clients, [
              'case_type_id' => wf_crm_aval($this->data['case'][$n], 'case:1:case_type_id'),
              'status_id' => $this->data['case'][$n]['existing_case_status']
            ]);
            if ($item) {
              $this->ent['case'][$n] = ['id' => $item['id']];
            }
          }
        }
      }
    }
  }

  /**
   * Find activity ids based on url input or "existing activity" settings
   */
  private function findExistingActivities() {
    // Support legacy url param
    if (empty($_GET["activity1"]) && !empty($_GET["aid"])) {
      $_GET["activity1"] = $_GET["aid"];
    }
    for ($n = 1; $n <= $this->data['activity']['number_of_activity']; ++$n) {
      if (!empty($this->data['activity'][$n]['activity'][1]['target_contact_id'])) {
        $targets = [];
        foreach ($this->data['activity'][$n]['activity'][1]['target_contact_id'] as $c) {
          if (!empty($this->ent['contact'][$c]['id'])) {
            $targets[] = $this->ent['contact'][$c]['id'];
          }
        }
        if ($targets) {
          if (isset($_GET["activity$n"]) && wf_crm_is_positive($_GET["activity$n"])) {
            $id = $_GET["activity$n"];
            $item = wf_civicrm_api('activity', 'getsingle', ['id' => $id, 'return' => ['target_contact_id']]);
            if (array_intersect($targets, $item['target_contact_id'])) {
              $this->ent['activity'][$n] = ['id' => $id];
            }
          }
          elseif (!empty($this->data['activity'][$n]['existing_activity_status'])) {
            // If the activity type hasn't been set, bail.
            if (empty($this->data['activity'][$n]['activity'][1]['activity_type_id'])) {
              watchdog('webform_civicrm', "Activity type to update hasn't been set, so won't try to update activity. location = %1, webform activity number : %2", ['%1' => request_uri(), '%2' => $n], WATCHDOG_ERROR);
              continue;
            }
            // The api doesn't accept an array of target contacts so we'll do it as a loop
            // If targets has more than one entry, the below could result in the wrong activity getting updated.
            foreach ($targets as $cid) {
              $params = [
                'sequential' => 1,
                'target_contact_id' => $cid,
                'status_id' => ['IN' => (array)$this->data['activity'][$n]['existing_activity_status']],
                'is_deleted' => '0',
                'is_current_revision' => '1',
                'options' => ['limit' => 1],
              ];
              $params['activity_type_id'] = $this->data['activity'][$n]['activity'][1]['activity_type_id'];
              $items = wf_crm_apivalues('activity', 'get', $params);
              if (isset($items[0]['id'])) {
                $this->ent['activity'][$n] = ['id' => $items[0]['id']];
                break;
              }
            }
          }
        }
      }
    }
  }

  /**
   * Find grant ids based on url input or "existing grant" settings
   */
  private function findExistingGrants() {
    for ($n = 1; $n <= $this->data['grant']['number_of_grant']; ++$n) {
      if (!empty($this->data['grant'][$n]['grant'][1]['contact_id'])) {
        $cid = $this->ent['contact'][$this->data['grant'][$n]['grant'][1]['contact_id']]['id'];
        if ($cid) {
          if (isset($_GET["grant$n"]) && wf_crm_is_positive($_GET["grant$n"])) {
            $id = $_GET["grant$n"];
            $item = wf_civicrm_api('grant', 'getsingle', ['id' => $id]);
            if ($cid == $item['contact_id']) {
              $this->ent['grant'][$n] = ['id' => $id];
            }
          }
          elseif (!empty($this->data['grant'][$n]['existing_grant_status'])) {
            $params = [
              'sequential' => 1,
              'contact_id' => $cid,
              'status_id' => ['IN' => (array)$this->data['grant'][$n]['existing_grant_status']],
              'options' => ['limit' => 1],
            ];
            if (!empty($this->data['grant'][$n]['grant'][1]['grant_type_id'])) {
              $params['grant_type_id'] = $this->data['grant'][$n]['grant'][1]['grant_type_id'];
            }
            $items = wf_crm_apivalues('grant', 'get', $params);
            if (isset($items[0]['id'])) {
              $this->ent['grant'][$n] = ['id' => $items[0]['id']];
            }
          }
        }
      }
    }
  }

  /**
   * Populate existing entity data
   * @param string $type entity type (activity, case, grant)
   */
  private function populateExistingEntity($type) {
    $items = [];
    foreach ($this->ent[$type] as $key => $item) {
      if (!empty($item['id'])) {
        $items[$key] = $item['id'];
      }
    }
    if ($items) {
      $values = wf_crm_apivalues($type, 'get', ['id' => ['IN' => array_values($items)]]);
      foreach ($items as $n => $id) {
        if (isset($values[$id])) {
          // Load core + custom data
          $this->info[$type][$n] = [$type => [1 => $values[$id]]] + $this->getCustomData($id, $type);
          // Load file attachments
          if (!empty($this->all_sets["{$type}upload"])) {
            foreach ($this->getAttachments($type, $id) as $f => $file) {
              $this->info[$type][$n]["{$type}upload"][1]["file_$f"] = $file['file_id'];
            }
          }
          // Load tags
          $tags = NULL;
          foreach (array_keys($this->enabled) as $fid) {
            if (strpos($fid, "civicrm_{$n}_{$type}_1_{$type}_tag") === 0) {
              $tags = $tags ?? wf_crm_apivalues('EntityTag', 'get', ['entity_id' => $id, 'entity_table' => "civicrm_" . $type, 'sequential' => 1], 'tag_id');
              $this->info[$type][$n][$type][1][str_replace("civicrm_{$n}_{$type}_1_{$type}_", '', $fid)] = $tags;
            }
          }
        }
      }
    }
  }

  /**
   * Wrapper for drupal_set_message
   * Ensures we only set the message on the first page of the node display
   * @param $message
   * @param string $type
   */
  function setMessage($message, $type='status') {
    if (node_is_page($this->node) && empty($_POST)) {
      drupal_set_message($message, $type, FALSE);
    }
  }

  /**
   * Displays the admin-defined message with "not you?" link to known contacts
   *
   * @param string $message
   *   Raw message with tokens
   * @param array $contact
   *   CiviCRM contact array
   */
  private function showNotYouMessage($message, $contact) {
    $message = $this->replaceTokens($message, $contact);
    preg_match_all('#\{([^}]+)\}#', $message, $matches);
    if (!empty($matches[0])) {
      $q = $_GET;
      unset($q['q'], $q['cs'], $q['cid'], $q['cid1']);
      if (empty($_GET['cid']) && empty($_GET['cid1'])) {
        $q['cid1'] = 0;
      }
      foreach ($matches[0] as $pos => $match) {
        $link = l($matches[1][$pos], $_GET['q'], ['query' => $q, 'alias' => TRUE]);
        $message = str_replace($match, $link, $message);
      }
    }
    $this->setMessage($message);
  }

  /**
   * Token replacement for form messages
   *
   * @param $str
   *   Raw message with tokens
   * @param $contact
   *   CiviCRM contact array
   * @return mixed
   */
  private function replaceTokens($str, $contact) {
    $tokens = wf_crm_get_fields('tokens');
    $values = [];
    foreach ($tokens as $k => &$t) {
      if (empty($contact[$k])) {
        $contact[$k] = '';
      }
      $value = $contact[$k];
      if (is_array($value)) {
        $value = implode(', ', $value);
      }
      $values[] = implode(' &amp; ', wf_crm_explode_multivalue_str(check_plain($value)));
      $t = "[$t]";
    }
    return str_ireplace($tokens, $values, $str);
  }

}
