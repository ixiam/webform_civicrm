<?php

/**
 * @file
 * Webform CiviCRM module's front-end form preprocessor.
 */

module_load_include('inc', 'webform_civicrm', 'includes/wf_crm_webform_base');

class wf_crm_webform_preprocess extends wf_crm_webform_base {
  private $form;
  private $form_state;
  private $info = array();
  private $id = array();

  function __construct(&$form, &$form_state) {
    civicrm_initialize();
    $this->form = &$form;
    $this->form_state = &$form_state;
    $this->node = $form['#node'];
    $this->settings = $this->node->webform_civicrm;
    $this->data = $this->settings['data'];
    $this->id['cid'] = array();
    $this->enabled = wf_crm_enabled_fields($this->node);
  }

  /**
   * Alter front-end of webforms: Called by hook_form_alter() when rendering a civicrm-enabled webform
   * Add custom prefix.
   * Display messages.
   * Block users who should not have access.
   * Set webform default values.
   */
  public function alterForm() {
    $this->form['#attached']['js'][] = drupal_get_path('module', 'webform_civicrm') . '/js/webform_civicrm_forms.js';
    $this->form['#attached']['css'][] = drupal_get_path('module', 'webform_civicrm') . '/css/webform_civicrm_forms.css';
    $this->form['#validate'][] = 'wf_crm_validate';
    array_unshift($this->form['#submit'], 'wf_crm_storage');
    $config = CRM_Core_Config::singleton();
    // Variables to push to the client-side
    $js_vars = array();
    // JS Cache eliminates the need for most ajax state/province callbacks
    foreach ($this->data['contact'] as $c) {
      if (!empty($c['number_of_address'])) {
        $js_vars += array(
          'defaultCountry' => $config->defaultContactCountry,
          'defaultStates' => wf_crm_get_states($config->defaultContactCountry),
          'noCountry' => t('- First Choose a Country -'),
          'callbackPath' => url('webform-civicrm/js', array('alias' => TRUE)),
        );
        break;
      }
    }
    // Preprocess contribution page
    if (!empty($this->data['contribution'])) {
      CRM_Core_Resources::singleton()->addCoreResources();
      $page_id = $this->data['contribution'][1]['contribution'][1]['contribution_page_id'];
      $key = CRM_Core_Key::get('CRM_Contribute_Controller_Contribution', TRUE);
      $js_vars['contributionCallback'] = url('civicrm/contribute/transact', array('query' => array('reset' => 1, 'id' => $page_id, 'qfKey' => $key, 'snippet' => 4, 'type' => 1), 'alias' => TRUE));
    }
    if ($js_vars) {
      $this->form['#attached']['js'][] = array(
        'data' => array('webform_civicrm' => $js_vars),
        'type' => 'setting',
      );
    }
    // Keep track of cids across multipage forms
    if (!empty($this->form_state['values']['submitted']) && wf_crm_aval($this->form_state, 'webform:page_count') > 1) {
      foreach ($this->enabled as $k => $v) {
        if (substr($k, -8) == 'existing' && !empty($this->form_state['values']['submitted'][$v])) {
          list(, $c) = explode('_', $k);
          $cid_data["cid$c"] = $this->form_state['values']['submitted'][$v];
        }
      }
      if (!empty($cid_data)) {
        $this->form['#attributes']['data-civicrm-ids'] = json_encode($cid_data);
      }
    }
    // Early return if the form (or page) was already submitted
    if (wf_crm_aval($this->form_state, 'triggering_element:#id') == 'edit-previous'
    || (empty($this->form_state['rebuild']) && !empty($this->form_state['storage']))) {
      $this->fillForm($this->form['submitted']);
      return;
    }
    // If this is an edit op, use the original IDs and return
    if (isset($this->form['#submission']->sid)) {
      if (isset($this->form['#submission']->civicrm)) {
        $this->form_state['civicrm']['id']['cid'] = $this->form['#submission']->civicrm['contact_id'];
        $this->form_state['civicrm']['id']['act'][1] = $this->form['#submission']->civicrm['activity_id'];
        foreach ($this->form_state['civicrm']['id']['cid'] as $c => $cid) {
          $this->info['contact'][$c]['contact'][1]['existing'] = $cid;
        }
      }
      $this->fillForm($this->form['submitted']);
      return;
    }
    // If this form is already in-process, IDs will be stored
    if (!empty($this->form_state['civicrm'])) {
      $this->id = $this->form_state['civicrm']['id'];
    }
    // Search for existing contacts
    else {
      $count = count($this->data['contact']);
      for ($i = 1; $i <= $count; ++$i) {
        if ($existing_component = wf_crm_aval($this->enabled, "civicrm_{$i}_contact_1_contact_existing")) {
          $this->findContact($this->node->webform['components'][$existing_component]);
        }
      }
    }
    // Lookup activity if passed in url
    if (!empty($_GET['aid']) && !empty($this->id['cid']) && is_numeric($_GET['aid'])) {
      $result = wf_civicrm_api('activity', 'get', array('activity_id' => $_GET['aid'], 'return.target_contact_id' => 1, 'return.assignee_contact_id' => 1));
      if (isset($result['values'][$_GET['aid']])) {
        $act = $result['values'][$_GET['aid']];
        // Verify that this activity is the right type and that our contacts have some involvement in it
        if ($act['activity_type_id'] == $this->data['activity'][1]['activity'][1]['activity_type_id']) {
          foreach ($this->id['cid'] as $cid) {
            if ($act['source_contact_id'] == $cid || in_array($cid, $act['target_contact_id']) || in_array($cid, $act['assignee_contact_id'])) {
              $activity = array('activity' => array(1 => $result['values'][$_GET['aid']]));
              $custom = wf_crm_get_custom($_GET['aid'], 'activity');
              $this->info['activity'] = array(1 => ($activity + $custom));
              $this->id['act'][1] = $_GET['aid'];
              if (!empty($this->data['case'][1])) {
                $result = wf_civicrm_api('case', 'get', array('activity_id' => $this->id['act'][1]));
                if ($this->id['case'][1] = wf_crm_aval($result, 'id')) {
                  $custom = wf_crm_get_custom($this->id['case'][1], 'case');
                  $this->info['case'] = array(1 => (array('case' => array(1 => $result['values'][$result['id']])) + $custom));
                }
              }
              break;
            }
          }
        }
      }
    }
    // Search for case & activity
    else {
      if (!empty($this->data['case'][1])) {
        $case = wf_crm_case_find($this->data['case'][1], $this->id);
        if ($case) {
          $this->id['case'][1] = $case['id'];
          $custom = wf_crm_get_custom($case['id'], 'case');
          // Add case to $this->info in standard webform_civicrm format
          $this->info['case'] = array(1 => (array('case' => array(1 => $case)) + $custom));
        }
      }
      if ((!empty($this->id['cid'][1]) && empty($this->data['case'][1])) || (!empty($this->data['case'][1]) && !empty($this->id['case'][1]))) {
        if (!empty($this->data['activity'][1]['existing_activity_status'])) {
          $params = array(
            'activity_type_id' => $this->data['activity'][1]['activity'][1]['activity_type_id'],
            'status_id' => $this->data['activity'][1]['existing_activity_status'],
          );
          if (!empty($this->data['case'][1])) {
            $params['case_id'] = $this->id['case'][1];
          }
          else {
            $params['contact_id'] = $this->id['cid'][1];
          }
          $this->id['act'][1] = wf_crm_activity_find($params);
          if ($this->id['act'][1]) {
            $result = wf_civicrm_api('activity', 'get', array('activity_id' => $this->id['act'][1]));
            $activity = array('activity' => array(1 => $result['values'][$this->id['act'][1]]));
            $custom = wf_crm_get_custom($this->id['act'][1], 'activity');
            $this->info['activity'] = array(1 => ($activity + $custom));
          }
        }
      }
    }
    // Form alterations for unknown contacts
    if (empty($this->id['cid'][1])) {
      if ($this->settings['prefix_unknown']) {
        $this->form['#prefix'] = wf_crm_aval($this->form, '#prefix', '') . '<div class="webform-civicrm-prefix contact-unknown">' . nl2br($this->settings['prefix_unknown']) . '</div>';
      }
      if ($this->settings['block_unknown_users']) {
        $this->form['submitted']['#access'] = $this->form['actions']['#access'] = FALSE;
        drupal_set_message(t('Sorry, you do not have permission to access this form.'), 'warning', FALSE);
        return;
      }
    }
    // Check if events are open to registration and take appropriate action
    $events = array();
    $reg = wf_crm_aval($this->data, 'reg_options', array());
    if (!empty($this->data['participant_reg_type'])) {
      // Fetch events set in back-end
      $this->data += array('participant' => array());
      foreach ($this->data['participant'] as $e => $par) {
        if (!empty($par['participant'])) {
          foreach ($par['participant'] as $n => $p) {
            if (!empty($p['event_id'])) {
              // Handle multi-valued event selection
              foreach ((array) $p['event_id'] as $eid) {
                if ($eid = (int) $eid) {
                  $events[$eid]['ended'] = TRUE;
                  $events[$eid]['title'] = t('this event');
                  $events[$eid]['count'] = wf_crm_aval($events, "$eid:count", 0) + 1;
                  $events[$eid]['form'][] = array(
                    'contact' => $e,
                    'num' => $n,
                    'eid' => NULL
                  );
                }
              }
            }
          }
        }
      }
      // Add events exposed to the form
      foreach ($this->enabled as $field => $fid) {
        if (strpos($field, 'participant_event_id')) {
          foreach ($this->getExposedOptions($fid) as $p => $label) {
            list($eid) = explode('-', $p);
            $events[$eid]['ended'] = TRUE;
            $events[$eid]['title'] = $label;
            list(, $e, , $n) = explode('_', $field);
            $events[$eid]['form'][] = array(
              'contact' => $e,
              'num' => $n,
              'eid' => $p
            );
          }
        }
      }
      if ($events && (!empty($reg['show_remaining']) || !empty($reg['block_form']))) {
        wf_crm_event_info($events);
        foreach ($events as $eid => $event) {
          if ($event['ended']) {
            if (!empty($reg['show_remaining']) && empty($this->form_state['input']) && empty($this->form_state['storage'])) {
              drupal_set_message(t('Sorry, %event has ended.', array('%event' => $event['title'])), 'warning', FALSE);
            }
          }
          elseif ($event['full']) {
            if (!empty($reg['show_remaining']) && empty($this->form_state['input']) && empty($this->form_state['storage'])) {
              drupal_set_message('<em>' . $event['title'] . '</em>: ' . $event['full_message'], 'warning', FALSE);
            }
          }
          else {
            $reg['block_form'] = FALSE;
            if ($event['max_participants'] && empty($this->form_state['input']) && empty($this->form_state['storage'])
            && ($reg['show_remaining'] == 'always' || intval($reg['show_remaining']) >= $event['remaining'])) {
              drupal_set_message(format_plural($event['remaining'],
                '%event has 1 remaining space.',
                '%event has @count remaining spaces.',
                array('%event' => $event['title'])), 'status', FALSE);
            }
          }
        }
        if ($reg['block_form']) {
          $this->form['submitted']['#access'] = $this->form['actions']['#access'] = FALSE;
          return;
        }
      }
    }
    // Form alterations for known contacts
    foreach ($this->data['contact'] as $c => $contact) {
      if ($cid = wf_crm_aval($this->id['cid'], $c)) {
        // Retrieve contact data
        $this->info['contact'][$c] = $this->loadContact($c, $this->id['cid']);
        $this->info['contact'][$c]['contact'][1]['existing'] = $cid;
        // Retrieve participant data
        if ($events && ($c == 1 || $this->data['participant_reg_type'] == 'separate')) {
          $select = array('id', 'event_id', 'role_id', 'status_id');
          if (in_array('CiviCampaign', $config->enableComponents, TRUE)) {
            $select[] = 'campaign_id';
          }
          $dao = &CRM_Core_DAO::executeQuery('SELECT ' . implode(',', $select) . " FROM civicrm_participant WHERE contact_id = $cid AND event_id IN (" . implode(',', array_keys($events)) . ") AND status_id IN (SELECT id FROM civicrm_participant_status_type WHERE class <> 'Negative')");
          while ($dao->fetch()) {
            $par = array();
            foreach ($select as $sel) {
              $par['participant'][1][$sel] = $dao->$sel;
            }
            $par += wf_crm_get_custom($dao->id, 'Participant');
            foreach ($events[$dao->event_id]['form'] as $event) {
              if ($event['contact'] == $c) {
                $n = $event['contact'];
                $i = $event['num'];
                // Support multi-valued form elements
                $event_ids = wf_crm_aval($this->info, "participant:$n:participant:$i:event_id", array());
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
    }
    if (!empty($this->id['cid'][1])) {
      if ($this->settings['prefix_known']) {
        $this->form['#prefix'] = wf_crm_aval($this->form, '#prefix', '') . '<div class="webform-civicrm-prefix contact-known">' . nl2br(wf_crm_replace_tokens($this->settings['prefix_known'], $this->info['contact'][1]['contact'][1])) . '</div>';
      }
      if ($this->settings['message'] && empty($this->form_state['input']) && empty($this->form_state['storage'])) {
        $this->setMessage($this->settings['message'], $this->info['contact'][1]['contact'][1]);
      }
    }
    // Store ids
    $this->form_state['civicrm']['id'] = $this->id;
    // Set default values and other attributes for CiviCRM form elements
    // Passing $submitted helps avoid overwriting values that have been entered on a multi-step form
    $submitted = wf_crm_aval($this->form_state, 'values:submitted', array());
    $this->fillForm($this->form['submitted'], $submitted);
  }

  /**
   * Find an existing contact based on matching criteria
   * Used to populate a webform existing contact field
   *
   * @param array $component
   *   Webform component of type 'civicrm_contact'
   */
  function findContact($component) {
    module_load_include('inc', 'webform_civicrm', 'includes/contact_component');
    list(, $c, ) = explode('_', $component['form_key'], 3);
    $filters = wf_crm_search_filters($this->node, $component);
    // Start with the url - that trumps everything.
    if (isset($_GET["cid$c"]) || ($c == 1 && isset($_GET['cid']))) {
      $cid = isset($_GET["cid$c"]) ? $_GET["cid$c"] : $_GET['cid'];
      if (is_numeric($cid)) {
        $cid = (int) $cid;
        if ($cid === 0) {
          $this->id['cid'][$c] = $cid;
        }
        else {
          if (wf_crm_contact_access($component, $filters, $cid) !== FALSE) {
            $this->id['cid'][$c] = $cid;
          }
        }
      }
    }
    if (!isset($this->id['cid'][$c])) {
      $found = array();
      switch ($component['extra']['default']) {
        case 'user':
          $cid = wf_crm_user_cid();
          $found = ($c == 1 && $cid) ? array($cid) : array();
          break;
        case 'contact_id':
          if ($component['extra']['default_contact_id']) {
            $found = array($component['extra']['default_contact_id']);
          }
          break;
        case 'relationship':
          if (!empty($this->id['cid'][1])) {
            $found = wf_crm_find_relations($this->id['cid'][1], $component['extra']['default_relationship']);
          }
          break;
        case 'auto':
          $component['extra']['allow_create'] = FALSE;
          $found = array_keys(wf_crm_contact_search($this->node, $component, $filters));
          break;
      }
      if ($component['extra']['randomize']) {
        shuffle($found);
      }
      if (in_array($component['extra']['default'], array('user', 'contact_id'))) {
        $dupes_allowed = TRUE;
      }
      else {
        $dupes_allowed = $component['extra']['dupes_allowed'];
      }
      foreach ($found as $cid) {
        // Don't pick the same contact twice unless explicitly told to do so
        if (!in_array($cid, $this->id['cid']) || $dupes_allowed) {
          // Check filters except for 'auto' which already applied them
          if ($component['extra']['default'] == 'auto' || wf_crm_contact_access($component, $filters, $cid) !== FALSE) {
            $this->id['cid'][$c] = $cid;
            break;
          }
        }
      }
    }
    // Identify contact 1 as acting user if not already logged in
    if (!empty($this->id['cid'][1]) && $c == 1 && user_is_anonymous()) {
      CRM_Core_DAO::executeQuery('SET @civicrm_user_id = %1', array(1 => array($this->id['cid'][1], 'Integer')));
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
  private function fillForm(&$elements, $submitted = array()) {
    $sp = CRM_Core_DAO::VALUE_SEPARATOR;
    $fields = wf_crm_get_fields();
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
        if ($field = wf_crm_aval($fields, $table . '_' . $name)) {
          $component = $element['#webform_component'];
          $element['#attributes']['class'][] = 'civicrm-enabled';
          if (!empty($field['data_type'])) {
            $dt = $element['#civicrm_data_type'] = $field['data_type'];
            // Add CiviCRM JS to link fields
            if ($dt == 'Link' && substr($element['#type'], 0, 4) == 'text') {
              $element['#attributes']['onblur'] = "if (this.value == 'http://') {this.value = '';}";
              $element['#attributes']['onfocus'] = "if (this.value == '') {this.value = 'http://';}";
            }
          }
          // Provide live options from the Civi DB
          elseif (!empty($component['extra']['civicrm_live_options']) && isset($element['#options'])) {
            $params = array('extra' => wf_crm_aval($field, 'extra', array())) + $component;
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
            if (($element['#type'] == 'checkboxes' || !empty($element['#multiple'])) && !is_array($val)) {
              $val = explode($sp, trim($val, $sp));
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
            // Set data for contact image
            if ($name == 'image_URL') {
              if ($val && in_array($element['#type'], array('file', 'managed_file'))) {
                $js = "jQuery(function() {wfCivi.contactImage('$eid', '$val')});";
                $element['#attached']['js'][$js] = array('type' => 'inline');
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
            wf_crm_fill_contact_value($this->node, $component, $element);
          }
        }
      }
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
  private function setMessage($message, $contact) {
    $message = wf_crm_replace_tokens($message, $contact);
    preg_match_all('#\{([^}]+)\}#', $message, $matches);
    if (!empty($matches[0])) {
      $q = $_GET;
      unset($q['q'], $q['cs'], $q['cid'], $q['cid1']);
      if (empty($_GET['cid']) && empty($_GET['cid1'])) {
        $q['cid1'] = 0;
      }
      foreach ($matches[0] as $pos => $match) {
        $link = l($matches[1][$pos], $_GET['q'], array('query' => $q, 'alias' => TRUE));
        $message = str_replace($match, $link, $message);
      }
    }
    drupal_set_message($message);
  }

}