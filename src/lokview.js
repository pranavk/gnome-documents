/*
 * Copyright (c) 2013, 2014, 2015 Red Hat, Inc.
 *
 * Gnome Documents is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * Gnome Documents is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Gnome Documents; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

const LOKDocView = imports.gi.LOKDocView;
const WebKit = imports.gi.WebKit2;
const Soup = imports.gi.Soup;
const Gd = imports.gi.Gd;
const GdPrivate = imports.gi.GdPrivate;
const Gdk = imports.gi.Gdk;
const Gio = imports.gi.Gio;
const GLib = imports.gi.GLib;
const Gtk = imports.gi.Gtk;
const _ = imports.gettext.gettext;

const Lang = imports.lang;
const Mainloop = imports.mainloop;
const Signals = imports.signals;

const Application = imports.application;
const MainToolbar = imports.mainToolbar;
const Searchbar = imports.searchbar;
const Utils = imports.utils;
const View = imports.view;
const WindowMode = imports.windowMode;
const Documents = imports.documents;

const _BLANK_URI = "about:blank";

const LOKView = new Lang.Class({
    Name: 'LOKView',

    _init: function() {
        this._uri = null;

        this.widget = new Gtk.Overlay();
        this.widget.get_style_context().add_class('documents-scrolledwin');

        this.view  = LOKDocView.View.new('/opt/libreoffice/instdir/program', null, null);

        this._sw = new Gtk.ScrolledWindow({hexpand: true,
                                           vexpand: true});
        
        this._progressBar = new Gtk.ProgressBar({ halign: Gtk.Align.FILL,
                                                  valign: Gtk.Align.START });
        this._progressBar.get_style_context().add_class('osd');
        this.widget.add_overlay(this._progressBar);

        this.widget.add(this._sw);
        this._createView();
        
        this.widget.show_all();

        Application.documentManager.connect('load-started',
                                            Lang.bind(this, this._onLoadStarted));
        Application.documentManager.connect('load-finished',
                                            Lang.bind(this, this._onLoadFinished));

    },

    _onLoadStarted: function() {
        //this._editAction.enabled = false;
        //this._viewAction.enabled = false;
    },

    open_document_cb: function() {
        log ("i am calback");
        this.view.show();
    },
    
    _onLoadFinished: function(manager, doc, docModel) {
        this._reset();
        if (docModel == null && doc != null) {
            let location = doc.uri.replace ('file://', '');
            this.view.open_document(location, null, Lang.bind(this, this.open_document_cb), null);
        }
    },

    _reset: function () {
        this.view.destroy();
        this.view  = LOKDocView.View.new('/opt/libreoffice/instdir/program', null, null);
        this._createView();
    },

    _createView: function() {
        this._sw.add(this.view);
        this.view.connect('load-changed', Lang.bind(this, this._onProgressChanged));
    },

    _onProgressChanged: function() {
        log ("progress changed");
        this._progressBar.fraction = this.view.estimated_load_progress;
    },
});
Signals.addSignalMethods(LOKView.prototype);
