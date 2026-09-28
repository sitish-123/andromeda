//! `umbriel-link-apps`: the paired phone's apps, each opened in its own desktop window through scrcpy over wireless
//! debugging, the only way a phone app runs on a second display. `--list` and `--launch` work without a window.

mod adb;

use std::process::ExitCode;
use std::rc::Rc;

use anyhow::bail;
use clap::Parser;
use gtk4::prelude::*;
use gtk4::{gio, glib};

use adb::{App, Found};

#[derive(Parser)]
#[command(about = "Open a paired phone's apps in desktop windows")]
struct Cli {
    /// The phone's name, as Link shows it; any name when only one phone is reachable.
    name: String,
    /// Print the phone's apps as JSON lines and exit.
    #[arg(long)]
    list: bool,
    /// Open the app with this package name and exit.
    #[arg(long, value_name = "PACKAGE")]
    launch: Option<String>,
}

fn main() -> ExitCode {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).format_timestamp(None).init();
    match run(Cli::parse()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            log::error!("{error:#}");
            ExitCode::FAILURE
        }
    }
}

fn run(cli: Cli) -> anyhow::Result<()> {
    if !cli.list && cli.launch.is_none() {
        let app = gtk4::Application::builder().application_id("org.umbriel.LinkApps").build();
        let name = cli.name;
        app.connect_activate(move |app| window(app, &name));
        app.run_with_args::<&str>(&[]);
        return Ok(());
    }
    let Found::Serial(serial) = adb::find(&cli.name)? else {
        bail!("the phone is not paired for wireless debugging; run umbriel-link-apps without options to pair it");
    };
    let apps = adb::apps(&serial)?;
    if cli.list {
        for app in &apps {
            println!("{}", serde_json::json!({ "name": app.name, "package": app.package }));
        }
    }
    if let Some(package) = cli.launch {
        let app = apps.into_iter().find(|app| app.package == package);
        let app = app.unwrap_or_else(|| App { name: package.clone(), package });
        adb::launch(&serial, &app)?;
    }
    Ok(())
}

struct Ui {
    name: String,
    stack: gtk4::Stack,
    list: gtk4::ListBox,
    status: gtk4::Label,
    code: gtk4::Entry,
}

fn window(app: &gtk4::Application, name: &str) {
    let status = gtk4::Label::builder().wrap(true).xalign(0.0).css_classes(["dim-label"]).build();
    let list = gtk4::ListBox::builder().selection_mode(gtk4::SelectionMode::None).css_classes(["boxed-list"]).build();
    let search = gtk4::SearchEntry::builder().placeholder_text("Search apps").build();
    let apps_page = gtk4::Box::new(gtk4::Orientation::Vertical, 12);
    apps_page.append(&search);
    apps_page.append(&gtk4::ScrolledWindow::builder().child(&list).vexpand(true).build());
    let code =
        gtk4::Entry::builder().placeholder_text("Pairing code").input_purpose(gtk4::InputPurpose::Digits).build();
    let pair = gtk4::Button::with_label("Pair");
    pair.add_css_class("suggested-action");
    let pair_page = gtk4::Box::new(gtk4::Orientation::Vertical, 12);
    pair_page.append(
        &gtk4::Label::builder()
            .label(
                "Phone apps open through wireless debugging. On the phone, open Settings, Developer options, Wireless \
                 debugging; turn it on, tap Pair device with pairing code, and enter the code here. This is needed \
                 once.",
            )
            .wrap(true)
            .xalign(0.0)
            .build(),
    );
    pair_page.append(&code);
    pair_page.append(&pair);
    let stack = gtk4::Stack::new();
    stack.add_named(&gtk4::Spinner::builder().spinning(true).build(), Some("looking"));
    stack.add_named(&pair_page, Some("pair"));
    stack.add_named(&apps_page, Some("apps"));
    let content = gtk4::Box::builder()
        .orientation(gtk4::Orientation::Vertical)
        .spacing(12)
        .margin_top(12)
        .margin_bottom(12)
        .margin_start(12)
        .margin_end(12)
        .build();
    content.append(&stack);
    content.append(&status);
    let window = gtk4::ApplicationWindow::builder()
        .application(app)
        .title(format!("{name}: apps"))
        .default_width(420)
        .default_height(640)
        .child(&content)
        .build();
    let ui = Rc::new(Ui { name: name.to_owned(), stack, list, status, code });
    {
        let ui = ui.clone();
        search.connect_search_changed(move |search| {
            let query = search.text().to_lowercase();
            let mut row = ui.list.first_child();
            while let Some(widget) = row {
                widget.set_visible(query.is_empty() || widget.widget_name().to_lowercase().contains(&query));
                row = widget.next_sibling();
            }
        });
    }
    {
        let ui = ui.clone();
        pair.connect_clicked(move |_| pair_now(&ui));
    }
    refresh(&ui);
    window.present();
}

/// Finds the phone off the main thread, then shows its apps or the pairing page.
fn refresh(ui: &Rc<Ui>) {
    ui.stack.set_visible_child_name("looking");
    ui.status.set_text("Looking for the phone…");
    let ui = ui.clone();
    glib::spawn_future_local(async move {
        let name = ui.name.clone();
        let found = gio::spawn_blocking(move || {
            let found = adb::find(&name)?;
            let apps = match &found {
                Found::Serial(serial) => adb::apps(serial)?,
                Found::Unpaired { .. } => Vec::new(),
            };
            anyhow::Ok((found, apps))
        })
        .await;
        match found {
            Ok(Ok((Found::Serial(serial), apps))) => show_apps(&ui, &serial, apps),
            Ok(Ok((Found::Unpaired { .. }, _))) => {
                ui.status.set_text("");
                ui.stack.set_visible_child_name("pair");
            }
            Ok(Err(error)) => ui.status.set_text(&format!("{error:#}")),
            Err(_) => ui.status.set_text("looking for the phone failed"),
        }
    });
}

fn show_apps(ui: &Rc<Ui>, serial: &str, apps: Vec<App>) {
    while let Some(row) = ui.list.first_child() {
        ui.list.remove(&row);
    }
    ui.status.set_text(&format!("{} apps", apps.len()));
    for app in apps {
        let button = gtk4::Button::builder().label(&app.name).tooltip_text(&app.package).css_classes(["flat"]).build();
        button.set_widget_name(&app.name);
        let (serial, status) = (serial.to_owned(), ui.status.clone());
        button.connect_clicked(move |_| {
            if let Err(error) = adb::launch(&serial, &app) {
                status.set_text(&format!("{error:#}"));
            }
        });
        ui.list.append(&button);
    }
    ui.stack.set_visible_child_name("apps");
}

fn pair_now(ui: &Rc<Ui>) {
    let code = ui.code.text().trim().to_owned();
    ui.status.set_text("Pairing…");
    let (ui, name) = (ui.clone(), ui.name.clone());
    glib::spawn_future_local(async move {
        let paired = gio::spawn_blocking(move || match adb::find(&name)? {
            Found::Unpaired { pairing: Some(target) } => adb::pair(&target, &code),
            Found::Unpaired { pairing: None } => bail!("keep the phone's pairing code dialog open, then try again"),
            Found::Serial(_) => Ok(()),
        })
        .await;
        match paired {
            Ok(Ok(())) => refresh(&ui),
            Ok(Err(error)) => ui.status.set_text(&format!("{error:#}")),
            Err(_) => ui.status.set_text("pairing failed"),
        }
    });
}
